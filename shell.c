/* 
 * A tiny shell program with job control
 * 
 * Author: Weisheng Li
 * Email: wjl5238@psu.edu/ 
 */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <ctype.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <errno.h>

/* Misc manifest constants */
#define MAXLINE    1024   /* max line size */
#define MAXARGS     128   /* max args on a command line */
#define MAXJOBS      16   /* max jobs at any point in time */
#define MAXJID    1<<16   /* max job ID */

/* Job states */
#define UNDEF 0 /* undefined */
#define FG 1    /* running in foreground */
#define BG 2    /* running in background */
#define ST 3    /* stopped */

/* 
 * Jobs states: FG (foreground), BG (background), ST (stopped)
 * Job state transitions and enabling actions:
 *     FG -> ST  : ctrl-z
 *     ST -> FG  : fg command
 *     ST -> BG  : bg command
 *     BG -> FG  : fg command
 * At most 1 job can be in the FG state.
 */

/* Global variables */
extern char **environ;      /* defined in libc */
char prompt[] = "tsh> ";    /* command line prompt (DO NOT CHANGE) */
int verbose = 0;            /* if true, print additional output */
int nextjid = 1;            /* next job ID to allocate */
char sbuf[MAXLINE];         /* for composing sprintf messages */

struct job_t {              /* The job struct */
  pid_t pid;              /* job PID */
  int jid;                /* job ID [1, 2, ...] */
  int state;              /* UNDEF, BG, FG, or ST */
  char cmdline[MAXLINE];  /* command line */
};
struct job_t jobs[MAXJOBS]; /* The job list */
/* End global variables */


/* Function prototypes */

/* Here are the functions that you will implement */
void eval(char *cmdline);
int builtin_cmd(char **argv);
void do_bgfg(char **argv);
void waitfg(pid_t pid);

void sigchld_handler(int sig);
void sigtstp_handler(int sig);
void sigint_handler(int sig);

/* Here are helper routines that we've provided for you */
int parseline(const char *cmdline, char **argv); 
void sigquit_handler(int sig);

void clearjob(struct job_t *job);
void initjobs(struct job_t *jobs);
int maxjid(struct job_t *jobs); 
int addjob(struct job_t *jobs, pid_t pid, int state, char *cmdline);
int deletejob(struct job_t *jobs, pid_t pid); 
pid_t fgpid(struct job_t *jobs);
struct job_t *getjobpid(struct job_t *jobs, pid_t pid);
struct job_t *getjobjid(struct job_t *jobs, int jid); 
int pid2jid(pid_t pid); 
pid_t jid2pid(struct job_t *jobs, int jid);
void listjobs(struct job_t *jobs);
int str_isnumber(char* str);

void usage(void);
void unix_error(char *msg);
void app_error(char *msg);
typedef void handler_t(int);
handler_t *Signal(int signum, handler_t *handler);

/*
 * main - The shell's main routine 
 */
int main(int argc, char **argv) 
{
  char c;
  char cmdline[MAXLINE];
  int emit_prompt = 1; /* emit prompt (default) */

  /* Redirect stderr to stdout (so that driver will get all output
   * on the pipe connected to stdout) */
  dup2(1, 2);

  /* Parse the command line */
  while ((c = getopt(argc, argv, "hvp")) != EOF) {
    switch (c) {
      case 'h':             /* print help message */
        usage();
        break;
      case 'v':             /* emit additional diagnostic info */
        verbose = 1;
        break;
      case 'p':             /* don't print a prompt */
        emit_prompt = 0;  /* handy for automatic testing */
        break;
      default:
        usage();
    }
  }

  /* Install the signal handlers */

  /* These are the ones you will need to implement */
  Signal(SIGINT,  sigint_handler);   /* ctrl-c */
  Signal(SIGTSTP, sigtstp_handler);  /* ctrl-z */
  Signal(SIGCHLD, sigchld_handler);  /* Terminated or stopped child */

  /* This one provides a clean way to kill the shell */
  Signal(SIGQUIT, sigquit_handler); 

  /* Initialize the job list */
  initjobs(jobs);

  /* Execute the shell's read/eval loop */
  while (1) {
    /* Read command line */
    if (emit_prompt) {
      printf("%s", prompt);
      fflush(stdout);
    }
    if ((fgets(cmdline, MAXLINE, stdin) == NULL) && ferror(stdin))
      app_error("fgets error");
    if (feof(stdin)) { /* End of file (ctrl-d) */
      fflush(stdout);
      exit(0);
    }

    /* Evaluate the command line */
    eval(cmdline);
    fflush(stdout);
    fflush(stdout);
  } 

  exit(0); /* control never reaches here */
}

/* Wrapper function for fork() */
pid_t Fork(void) {
  pid_t pid;
  if ((pid = fork()) < 0)
    unix_error("Fork error");
  return pid;
}

/* 
 * eval - Evaluate the command line that the user has just typed in
 * 
 * If the user has requested a built-in command (quit, jobs, bg or fg)
 * then execute it immediately. Otherwise, fork a child process and
 * run the job in the context of the child. If the job is running in
 * the foreground, wait for it to terminate and then return.  Note:
 * each child process must have a unique process group ID so that our
 * background children don't receive SIGINT (SIGTSTP) from the kernel
 * when we type ctrl-c (ctrl-z) at the keyboard.  
 */
void eval(char *cmdline) 
{
  char *argv[MAXARGS] = {NULL};
  char buf[MAXLINE];
  int bg;
  int cmd;
  pid_t pid;
  sigset_t mask_all, mask_one, prev_one;

  sigfillset(&mask_all);
  sigemptyset(&mask_one);
  sigaddset(&mask_one, SIGCHLD);

  strcpy(buf, cmdline);
  
  /* parse the command line */
  bg = parseline(buf, argv);
  if (argv[0] == NULL)
    return;
 if (!(cmd = builtin_cmd(argv))) {
   sigprocmask(SIG_BLOCK, &mask_one, &prev_one); /* Block SIGCHLD */
   if ((pid = Fork()) == 0) {
     setpgid(0,0);
     sigprocmask(SIG_SETMASK, &prev_one, NULL); /* Unblock SIGCHLD in child process */
     if (execve(argv[0], argv, environ) < 0) {
       printf("%s: Command not found\n", argv[0]);
       exit(0);
     }
   }
   sigprocmask(SIG_BLOCK, &mask_all, NULL); /* Block all signal in parent process */
   addjob(jobs, pid, bg? BG:FG, buf);
   
   /* If the program should be run in foreground, wait until it terminate */
   if (!bg) {
     waitfg(pid); 
   }
   /* If it should be run in background, print out message
    * about the job and continue. */
   else {
     int newjobjid = nextjid - 1;
     printf("[%d] (%d) %s", (newjobjid > 0)? newjobjid:MAXJOBS, pid, cmdline);
   }  

   sigprocmask(SIG_SETMASK, &prev_one, NULL); /* Unblock for parent process */
 } else if (cmd == 1) {             /* if the first argv is "jobs" */
   listjobs(jobs);
 } else if (cmd == 2 || cmd == 3) { /* if the first argv is "fg" or "bg" */
   
   /* Error handling: not enough argument*/
   if (argv[1] == NULL) {
     printf("%s command requires PID or %%jobid argument\n", argv[0]);
     return;
   }   
   do_bgfg(argv);
 }
}
/* 
 * builtin_cmd - If the user has typed a built-in command then execute 
 *  it immediately
 */ 
int builtin_cmd(char **argv) {
  if (!strcmp(argv[0], "quit")) 
    exit(0);
  if (!strcmp(argv[0], "jobs"))
    return 1;
  if (!strcmp(argv[0], "bg"))
    return 2;
  if (!strcmp(argv[0], "fg"))
    return 3;
  return 0;
}

/* 
 * parseline - Parse the command line and build the argv array.
 * 
 * Characters enclosed in single quotes are treated as a single
 * argument.  Return true if the user has requested a BG job, false if
 * the user has requested a FG job.  
 */
int parseline(const char *cmdline, char **argv) 
{
  static char array[MAXLINE]; /* holds local copy of command line */
  char *buf = array;          /* ptr that traverses command line */
  char *delim;                /* points to first space delimiter */
  int argc;                   /* number of args */
  int bg;                     /* background job? */

  strcpy(buf, cmdline);
  buf[strlen(buf)-1] = ' ';  /* replace trailing '\n' with space */
  while (*buf && (*buf == ' ')) /* ignore leading spaces */
    buf++;

  /* Build the argv list */
  argc = 0;
  if (*buf == '\'') {
    buf++;
    delim = strchr(buf, '\'');
  }
  else {
    delim = strchr(buf, ' ');
  }

  while (argc < MAXARGS-1 && delim) {
    argv[argc++] = buf;
    *delim = '\0';
    buf = delim + 1;
    while (*buf && (*buf == ' ')) /* ignore spaces */
      buf++;

    if (*buf == '\'') {
      buf++;
      delim = strchr(buf, '\'');
    }
    else {
      delim = strchr(buf, ' ');
    }
  }
  if (delim) {
    fprintf(stderr, "Too many arguments.\n");
    argc = 0; //treat it as an empty line.
  }
  argv[argc] = NULL;

  if (argc == 0)  /* ignore blank line */
    return 1;

  /* should the job run in the background? */
  if ((bg = (*argv[argc-1] == '&')) != 0) {
    argv[--argc] = NULL;
  }
  return bg;
}

/* 
 * do_bgfg - Execute the builtin bg and fg commands
 */
void do_bgfg(char **argv) 
{
  char* jobArgument = argv[1];
  if (!str_isnumber(jobArgument+1)) {
    printf("%s: argument must be a PID or %%jobid\n", argv[0]);
    return;
  }
  int bg = !strcmp(argv[0], "bg"), isPID = (jobArgument[0] == '%')? 0:1; 
  /* Get the pid of the job specified in the arguments. */
  pid_t pid = (isPID)? atoi(jobArgument):jid2pid(jobs, atoi(jobArgument + 1));
  
  sigset_t mask, prev;
  sigfillset(&mask);

  /* Retrieve the pointer points to the job by pid*/
  struct job_t* job = getjobpid(jobs, pid); 

  /* Exception Handling */
  if (pid == 0) { /* When the input jid cannot be convert to pid */
    printf("%%%d: No such job\n", atoi(jobArgument + 1));
    return;
  } else if (job == NULL) { /* When the pid cannot be mapped to a job */
    printf("(%d): No such process\n", pid);
    return;
  }

  if (bg) { /* When the command is bg*/
    if (job == NULL) return;

    sigprocmask(SIG_BLOCK, &mask, &prev); /* Block all signals */
    (*job).state = BG;
    kill(-pid, SIGCONT);
    printf("[%d] (%d) %s", pid2jid(pid), pid, (*job).cmdline);
    sigprocmask(SIG_SETMASK, &prev, NULL); /* Unblock all signals */
  } else { /* When the command is fg*/
    if (job == NULL) return;

    sigprocmask(SIG_BLOCK, &mask, &prev); /* Block all signals */
    if ((*job).state == BG) {
      (*job).state = FG;
      waitfg(pid);
    } else if ((*job).state == ST) {
      (*job).state = FG;
      kill(-pid, SIGCONT);
      waitfg(pid);
    }
    sigprocmask(SIG_SETMASK, &prev, NULL); /* Unblock all signals */
  }
}

/* 
 * waitfg - Block until process pid is no longer the foreground process
 */
void waitfg(pid_t pid)
{
  int fgpid_value;
  sigset_t none_block, block_all, prev;
  sigfillset(&block_all);
  sigemptyset(&none_block);
  /* Suspend the process until foreground job was terminated */
  sigprocmask(SIG_BLOCK, &block_all, &prev); /* Block all signals */
  while ((fgpid_value = fgpid(jobs)))  
    sigsuspend(&none_block);
  sigprocmask(SIG_SETMASK, &prev, NULL);     /* Unblock all signals */
}

/*****************
 * Signal handlers
 *****************/

/* 
 * sigchld_handler - The kernel sends a SIGCHLD to the shell whenever
 *     a child job terminates (becomes a zombie), or stops because it
 *     received a SIGSTOP or SIGTSTP signal. The handler reaps all
 *     available zombie children, but doesn't wait for any other
 *     currently running children to terminate.  
 */
void sigchld_handler(int sig) 
{
  int olderrno = errno, status;
  sigset_t mask_all, prev_all;
  pid_t pid;

  sigfillset(&mask_all);

  /* Use a while loop with waitpid to handle terminated 
   * and stopped child process */
  while ((pid = waitpid(-1, &status, WNOHANG|WUNTRACED)) > 0) {
    sigprocmask(SIG_BLOCK, &mask_all, &prev_all); /* Block all signals */
    if (WIFSIGNALED(status)) {
      printf("Job [%d] (%d) terminated by signal 2\n", pid2jid(pid), pid);  
      deletejob(jobs, pid);
    } 
    else if (WIFSTOPPED(status)) {
      printf("Job [%d] (%d) stopped by signal 20\n", pid2jid(pid), pid);  
      (*getjobpid(jobs, pid)).state = ST;
    }
    else if (WIFEXITED(status)) {
      deletejob(jobs, pid);
    }
    sigprocmask(SIG_SETMASK, &prev_all, NULL);  /* Unblock all signals */
  }
  errno = olderrno; 
}

/* 
 * sigint_handler - The kernel sends a SIGINT to the shell whenver the
 *    user types ctrl-c at the keyboard.  Catch it and send it along
 *    to the foreground job.  
 */
void sigint_handler(int sig) 
{
  int olderrno = errno, fgpid_value;
  sigset_t mask, prev;

  sigfillset(&mask);
  /* If there is a fg job, send SIGINT to it */
  if ((fgpid_value = fgpid(jobs))) {
    sigprocmask(SIG_BLOCK, &mask, &prev); /* Block all signals */
    kill(-fgpid_value, SIGINT);
    sigprocmask(SIG_SETMASK, &prev, NULL);/* Unblock all signals */
  }  
  errno = olderrno;  
}

/*
 * sigtstp_handler - The kernel sends a SIGTSTP to the shell whenever
 *     the user types ctrl-z at the keyboard. Catch it and suspend the
 *     foreground job by sending it a SIGTSTP.  
 */
void sigtstp_handler(int sig) 
{
  int olderrno = errno, fgpid_value;
  sigset_t mask, prev;

  sigfillset(&mask);
  /* If there is a fg job, sent SIGTSTP to it*/
  if ((fgpid_value = fgpid(jobs))) {
    sigprocmask(SIG_BLOCK, &mask, &prev); /* Block all signals */
    kill(-fgpid_value, SIGTSTP);
    sigprocmask(SIG_SETMASK, &prev, NULL);/* Unblock all signals*/
  }
  errno = olderrno;
}

/*********************
 * End signal handlers
 *********************/

/***********************************************
 * Helper routines that manipulate the job list
 **********************************************/

/* clearjob - Clear the entries in a job struct */
void clearjob(struct job_t *job) {
  job->pid = 0;
  job->jid = 0;
  job->state = UNDEF;
  job->cmdline[0] = '\0';
}

/* initjobs - Initialize the job list */
void initjobs(struct job_t *jobs) {
  int i;

  for (i = 0; i < MAXJOBS; i++)
    clearjob(&jobs[i]);
}

/* maxjid - Returns largest allocated job ID */
int maxjid(struct job_t *jobs) 
{
  int i, max=0;

  for (i = 0; i < MAXJOBS; i++)
    if (jobs[i].jid > max)
      max = jobs[i].jid;
  return max;
}

/* addjob - Add a job to the job list */
int addjob(struct job_t *jobs, pid_t pid, int state, char *cmdline) 
{
  int i;

  if (pid < 1)
    return 0;

  for (i = 0; i < MAXJOBS; i++) {
    if (jobs[i].pid == 0) {
      jobs[i].pid = pid;
      jobs[i].state = state;
      jobs[i].jid = nextjid++;
      if (nextjid > MAXJOBS)
        nextjid = 1;
      strcpy(jobs[i].cmdline, cmdline);
      if(verbose){
        printf("Added job [%d] %d %s\n", jobs[i].jid, jobs[i].pid, jobs[i].cmdline);
      }
      return 1;
    }
  }
  printf("Tried to create too many jobs\n");
  return 0;
}

/* deletejob - Delete a job whose PID=pid from the job list */
int deletejob(struct job_t *jobs, pid_t pid) 
{
  int i;

  if (pid < 1)
    return 0;

  for (i = 0; i < MAXJOBS; i++) {
    if (jobs[i].pid == pid) {
      clearjob(&jobs[i]);
      nextjid = maxjid(jobs)+1;
      return 1;
    }
  }
  return 0;
}

/* fgpid - Return PID of current foreground job, 0 if no such job */
pid_t fgpid(struct job_t *jobs) {
  int i;

  for (i = 0; i < MAXJOBS; i++)
    if (jobs[i].state == FG)
      return jobs[i].pid;
  return 0;
}

/* getjobpid  - Find a job (by PID) on the job list */
struct job_t *getjobpid(struct job_t *jobs, pid_t pid) {
  int i;

  if (pid < 1)
    return NULL;
  for (i = 0; i < MAXJOBS; i++)
    if (jobs[i].pid == pid)
      return &jobs[i];
  return NULL;
}

/* getjobjid  - Find a job (by JID) on the job list */
struct job_t *getjobjid(struct job_t *jobs, int jid) 
{
  int i;

  if (jid < 1)
    return NULL;
  for (i = 0; i < MAXJOBS; i++)
    if (jobs[i].jid == jid)
      return &jobs[i];
  return NULL;
}

/* pid2jid - Map process ID to job ID */
int pid2jid(pid_t pid) 
{
  int i;

  if (pid < 1)
    return 0;
  for (i = 0; i < MAXJOBS; i++)
    if (jobs[i].pid == pid) {
      return jobs[i].jid;
    }
  return 0;
}

/* jid2pid - Map job ID to process ID, return 0 if encounter error */
pid_t jid2pid(struct job_t *jobs, int jid) {
  int i;
  pid_t pid = 0;

  if (jid < 1 || jid > 16)
    return pid;
  for (i = 0; i < MAXJOBS; i++) {
    if (jobs[i].jid == jid) {
      pid = jobs[i].pid;
      break;
    }
  }
  return pid;
}

/* listjobs - Print the job list */
void listjobs(struct job_t *jobs) 
{
  int i;

  for (i = 0; i < MAXJOBS; i++) {
    if (jobs[i].pid != 0) {
      printf("[%d] (%d) ", jobs[i].jid, jobs[i].pid);
      switch (jobs[i].state) {
        case BG: 
          printf("Running ");
          break;
        case FG: 
          printf("Foreground ");
          break;
        case ST: 
          printf("Stopped ");
          break;
        default:
          printf("listjobs: Internal error: job[%d].state=%d ", 
              i, jobs[i].state);
      }
      printf("%s", jobs[i].cmdline);
    }
  }
}
/******************************
 * end job list helper routines
 ******************************/


/***********************
 * Other helper routines
 ***********************/

/*
 * usage - print a help message
 */
void usage(void) 
{
  printf("Usage: shell [-hvp]\n");
  printf("   -h   print this message\n");
  printf("   -v   print additional diagnostic information\n");
  printf("   -p   do not emit a command prompt\n");
  exit(1);
}

/*
 * unix_error - unix-style error routine
 */
void unix_error(char *msg)
{
  fprintf(stdout, "%s: %s\n", msg, strerror(errno));
  exit(1);
}

/*
 * app_error - application-style error routine
 */
void app_error(char *msg)
{
  fprintf(stdout, "%s\n", msg);
  exit(1);
}

/*
 * Signal - wrapper for the sigaction function
 */
handler_t *Signal(int signum, handler_t *handler) 
{
  struct sigaction action, old_action;

  action.sa_handler = handler;  
  sigemptyset(&action.sa_mask); /* block sigs of type being handled */
  action.sa_flags = SA_RESTART; /* restart syscalls if possible */

  if (sigaction(signum, &action, &old_action) < 0)
    unix_error("Signal error");
  return (old_action.sa_handler);
}

/*
 * sigquit_handler - The driver program can gracefully terminate the
 *    child shell by sending it a SIGQUIT signal.
 */
void sigquit_handler(int sig) 
{
  printf("Terminating after receipt of SIGQUIT signal\n");
  exit(1);
}

/* str_isnumber - if the string is a number, return 1. Otherwise return 0.
 */ 
int str_isnumber(char* str) {
  int i, isEmpty = 1;
  for (i = 0; str[i] != 0; i++) {
    isEmpty = 0;
    if (!isdigit(str[i])) return 0;
  }
  return (isEmpty)? 0:1;
}

