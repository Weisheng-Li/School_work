////////////////////////////////////////////////////////////////////////////////
//
//  File           : cart_driver.c
//  Description    : This is the implementation of the standardized IO functions
//                   for used to access the CART storage system.
//
//  Author         : Weisheng Li 
//  PSU email      : wjl5238@psu.edu
//

// Includes
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdio.h>

// Project Includes
#include "cart_driver.h"
#include "cart_controller.h"

// I always made array slightly larger than requirement
// just to prevent some errors.  
#define MAX_NAME_LENGTH 136 
#define MAX_FILE_SIZE 102
#define MAX_FILES 1030

//
// Implementation

// declare file_handle, element of inner data structure
struct file_handle {
  char file_name[MAX_NAME_LENGTH];   // A string to contain file name
  int16_t open_status;               // 0 if closed, 1 if opened 
  int16_t used_carts[3];             // first element is the id of the cart where this file located,
                                     // the second element is the starting frame of this file
  int32_t file_pos;                  // indicate the file position, range from 0 to 102400;.
  int32_t file_size;                 // the number of bytes contains in the file.
};

// Global variables
struct file_handle VnodeTable[MAX_FILES];
int16_t next_fd = 0;    // index of next filehandler in vnodeTable       

// Helper functions

// initialize internal data structure
void init_VnodeTable(void) {
  int i;
  for (i = 0; i < MAX_FILES; i++) {
    // set every field in stuct 0
    VnodeTable[i].open_status = 0;
    VnodeTable[i].file_pos = 0;
    VnodeTable[i].file_size = 0;
    memset(&(VnodeTable[i].file_name), 0, MAX_NAME_LENGTH);
    memset(&(VnodeTable[i].used_carts), 0, 3*sizeof(int16_t));
  } 
}

// given values of different registers, return a corresponding 64 bit command
CartXferRegister Generate_cmd(int64_t KY1, int64_t CT1, int64_t FM1) {
  int64_t cmd = 0;
  cmd |= KY1 << 56;
  cmd |= CT1 << 31;
  cmd |= FM1 << 15;
  return cmd;
}

// given a cmd return from cart_io_bus, retrieve the RT bit
int getRT(CartXferRegister cmd) {
  int64_t mask = 1;
  cmd >>= 47;        // move RT bit to the right most bit
  return cmd & mask; // then & 0x00000..1 to inactivate all bits except RT
}

// Check the status of given file in VnodeTable 
// status could be: don't exist, exist & open, exist & close
int find_file(char* filename) {
  int i;
  // loop through all existing files in VnodeTable 
  for (i = 0; (VnodeTable[i].file_name)[0] != 0; i++) {
    char* temp_fn = &((VnodeTable[i].file_name)[0]);

    // if an existing file has the same file name, check it's open_status 
    if (strcmp(temp_fn, filename) == 0) {
      return (VnodeTable[i].open_status)?-2:i; // -2: exist&open, fd(non_negative int): exist&close
    }
  }
  assert(i < MAX_FILES);
  return -1;  // -1: don't exist
} 

////////////////////////////////////////////////////////////////////////////////
//
// Function     : cart_poweron
// Description  : Startup up the CART interface, initialize filesystem
//
// Inputs       : none
// Outputs      : 0 if successful, -1 if failure

int32_t cart_poweron(void) {
  CartXferRegister return_cmd, sent_cmd;

  // sent CART_OP_INITMS
  sent_cmd = Generate_cmd(CART_OP_INITMS, 0, 0);
  return_cmd = cart_io_bus(sent_cmd, NULL);
  if (getRT(return_cmd) != 0) return -1;

  // load and zero out all cartridge
  int i;
  for (i = 0; i < 64; i++) {
    // Load Cart
    sent_cmd = Generate_cmd(CART_OP_LDCART, i, 0); 
    return_cmd = cart_io_bus(sent_cmd, NULL);
    if (getRT(return_cmd) != 0) return -1;

    // Zero the Cart
    sent_cmd = Generate_cmd(CART_OP_BZERO, 0, 0);
    return_cmd = cart_io_bus(sent_cmd, NULL);
    if (getRT(return_cmd) != 0) return -1; 
  }

  // initialize inner data structure (VnodeTable)
  init_VnodeTable(); 

  return(0);
}

////////////////////////////////////////////////////////////////////////////////
//
// Function     : cart_poweroff
// Description  : Shut down the CART interface, close all files
//
// Inputs       : none
// Outputs      : 0 if successful, -1 if failure

int32_t cart_poweroff(void) {
  CartXferRegister sent_cmd, return_cmd;

  // Turn off the CART system
  sent_cmd = Generate_cmd(CART_OP_POWOFF, 0, 0);
  return_cmd = cart_io_bus(sent_cmd, NULL);
  if (getRT(return_cmd) != 0) return -1; 

  // Return successfully
  return(0);
}

////////////////////////////////////////////////////////////////////////////////
//
// Function     : cart_open
// Description  : This function opens the file and returns a file handle
//
// Inputs       : path - filename of the file to open
// Outputs      : file handle if successful, -1 if failure

int16_t cart_open(char *path) {
  int16_t rt_fd;
  switch (rt_fd = find_file(path)) {
    case -1:   // file doesn't exist

      // then set up a new file handler for it
      strncpy(&((VnodeTable[next_fd].file_name)[0]), path, MAX_NAME_LENGTH);
      VnodeTable[next_fd].open_status = 1;
      VnodeTable[next_fd].file_pos = 0;
      VnodeTable[next_fd].file_size = 0;

      // new file would be store in cart ((i+1)/10), since each cart contains up to 10 file in my driver.
      // I use i + 1 instead of i because i start from 0.
      (VnodeTable[next_fd].used_carts)[0] = ((next_fd + 1) / 10); 

      // The first frame occupied by this file would be (i % 10) * MAX_FILE_SIZE
      (VnodeTable[next_fd].used_carts)[1] = (next_fd % 10) * MAX_FILE_SIZE;
      
      next_fd++;
      return next_fd-1;  

    case -2:   // file exist & open
      printf("Invalid Command: cannot open a file twice.");
      return -1;
    
    default:   // file exist & closed
      VnodeTable[rt_fd].open_status = 1;
      VnodeTable[rt_fd].file_pos = 0;      
      return rt_fd;
  }
}
////////////////////////////////////////////////////////////////////////////////
//
// Function     : cart_close
// Description  : This function closes the file
//
// Inputs       : fd - the file descriptor
// Outputs      : 0 if successful, -1 if failure

int16_t cart_close(int16_t fd) {
  // check if the given fd is valid.
  if (fd >= next_fd) {
    printf("Invalide file handler.");
    return -1;
  }

  // check if the file is open
  if (VnodeTable[fd].open_status == 0) return -1;

  // close the file and set file_pos back to 0
  VnodeTable[fd].open_status = 0;
  VnodeTable[fd].file_pos = 0;

  // Return successfully
  return (0);
}

////////////////////////////////////////////////////////////////////////////////
//
// Function     : cart_read
// Description  : Reads "count" bytes from the file handle "fh" into the 
//                buffer "buf"
//
// Inputs       : fd - filename of the file to read from
//                buf - pointer to buffer to read into
//                count - number of bytes to read
// Outputs      : bytes read if successful, -1 if failure

int32_t cart_read(int16_t fd, void *buf, int32_t count) {
  // error checking
  if (fd >= next_fd) return -1;
  if (VnodeTable[fd].open_status == 0) return -1;

  CartXferRegister sent_cmd, return_cmd;

  // load the cart in which the file stores
  sent_cmd = Generate_cmd(CART_OP_LDCART, (VnodeTable[fd].used_carts)[0], 0);
  return_cmd = cart_io_bus(sent_cmd, NULL);
  if (getRT(return_cmd) != 0) return -1; 

  // Use a while loop to read new frames until reading enough bytes
  char cart_buf[CART_FRAME_SIZE+1];   // 1024 byte and a tailing 0 to mark
                                      // the end of buffer. 
  memset(cart_buf, 0, CART_FRAME_SIZE+1);
    
  int32_t filepos = VnodeTable[fd].file_pos;
  int32_t filesize =  VnodeTable[fd].file_size;

  // If the function want to read more bytes the remaining 
  // bytes in cart_read, reset the count
  if (filepos + count > filesize) {
    count = filesize - filepos;
  }

  int32_t unread = count;

  while(unread != 0) {
    memset(cart_buf, 0, CART_FRAME_SIZE+1); // empty the local buffer
    if ((unread + (filepos % 1024)) <= 1024) {
      // get the frame where filepos locate
      int32_t frame = (filepos / 1024) + (VnodeTable[fd].used_carts)[1];


      // Read from this frame
      sent_cmd = Generate_cmd(CART_OP_RDFRME, 0, frame);
      return_cmd = cart_io_bus(sent_cmd, cart_buf);
      if (getRT(return_cmd) != 0) return -1; 

      // copy from cart_buf to buf (only copy the part
      // after file_pos)
      strncpy(buf, cart_buf + (filepos % 1024), unread);
      
      filepos += unread;
      unread = 0;
    }
    else { // when needs to read more than one frame
      // get the frame where filepos locate
      int32_t frame = (filepos / 1024) + (VnodeTable[fd].used_carts)[1];

      // Read from this frame
      sent_cmd = Generate_cmd(CART_OP_RDFRME, 0, frame);
      return_cmd = cart_io_bus(sent_cmd, cart_buf);
      if (getRT(return_cmd) != 0) return -1; 

      // Copy from cart_buf to buf
      int16_t r_bytes = 1024 - (filepos % 1024);  // number of bytes read in this iteration
      strncpy(buf, cart_buf + (filepos % 1024), r_bytes);

      // Update variables
      filepos += r_bytes;
      assert((filepos % 1024) == 0);
      unread -= r_bytes;
      buf = (char*)buf + r_bytes;
    } 
  }

  // Update file_pos in VnodeTable
  VnodeTable[fd].file_pos = filepos;

  // Return successfully
  return (count);
}

////////////////////////////////////////////////////////////////////////////////
//
// Function     : cart_write
// Description  : Writes "count" bytes to the file handle "fh" from the 
//                buffer  "buf"
//
// Inputs       : fd - filename of the file to write to
//                buf - pointer to buffer to write from
//                count - number of bytes to write
// Outputs      : bytes written if successful, -1 if failure

int32_t cart_write(int16_t fd, void *buf, int32_t count) {
  // error checking
  if (fd >= next_fd) return -1;
  if (VnodeTable[fd].open_status == 0) return -1;

  CartXferRegister sent_cmd, return_cmd; 
  int32_t filepos = VnodeTable[fd].file_pos;
  int32_t filesize = VnodeTable[fd].file_size;
  
  char cart_buf[CART_FRAME_SIZE + 1];
  memset(cart_buf, 0, CART_FRAME_SIZE + 1); 

  // load the cartridge
  sent_cmd = Generate_cmd(CART_OP_LDCART, (VnodeTable[fd].used_carts)[0], 0);
  return_cmd = cart_io_bus(sent_cmd, NULL);
  if (getRT(return_cmd) != 0) return -1; 

  // Check if the write command enlarge the file
  if (filepos + count > filesize) {
    filesize = filepos + count; 
  }

  int32_t unwritten = count;
  while (unwritten != 0) {
    memset(cart_buf, 0, CART_FRAME_SIZE + 1);
    if ((unwritten + (filepos % 1024)) <= 1024) {
      int32_t frame = (filepos / 1024) + (VnodeTable[fd].used_carts)[1];
      
      // Read the frame
      sent_cmd = Generate_cmd(CART_OP_RDFRME, 0, frame);
      return_cmd = cart_io_bus(sent_cmd, cart_buf);
      if (getRT(return_cmd) != 0) return -1; 

      // Write to the buffer returned by CART_OP_RDFRME
      int16_t fp = filepos % 1024; //file position within a frame
      strncpy(cart_buf + fp, buf, unwritten);
      assert(unwritten == strlen(buf));

      // Update the frame
      sent_cmd = Generate_cmd(CART_OP_WRFRME, 0, frame);
      return_cmd = cart_io_bus(sent_cmd, cart_buf);
      if (getRT(return_cmd) != 0) return -1; 
      
      filepos += unwritten; 
      unwritten = 0;
    }
    else {  // when need to write more than one frame
      int32_t frame = (filepos / 1024) + (VnodeTable[fd].used_carts)[1];
      
      // Read the frame
      sent_cmd = Generate_cmd(CART_OP_RDFRME, 0, frame);
      return_cmd = cart_io_bus(sent_cmd, cart_buf);
      if (getRT(return_cmd) != 0) return -1; 

      // Write to the buffer returned by CART_OP_RDFRME
      int16_t fp = filepos % 1024; //file position within a frame
      strncpy(cart_buf + fp, buf, 1024 - fp);
      assert(strlen(cart_buf) == 1024);

      // Update the frame
      sent_cmd = Generate_cmd(CART_OP_WRFRME, 0, frame);
      return_cmd = cart_io_bus(sent_cmd, cart_buf);
      if (getRT(return_cmd) != 0) return -1; 

      // Update variables
      filepos += 1024 - fp;
      assert((filepos % 1024) == 0);
      unwritten -= 1024 - fp;
      buf = (char*)buf + (1024 - fp);
    }
  }

  // Update file_pos & file_size
  VnodeTable[fd].file_pos = filepos;
  VnodeTable[fd].file_size = filesize; 

  // Return successfully
  return (count);
}

////////////////////////////////////////////////////////////////////////////////
//
// Function     : cart_seek
// Description  : Seek to specific point in the file
//
// Inputs       : fd - filename of the file to write to
//                loc - offfset of file in relation to beginning of file
// Outputs      : 0 if successful, -1 if failure

int32_t cart_seek(int16_t fd, uint32_t loc) {
  // return -1 if fd doesn't exist
  if (fd >= next_fd) return -1;

  // return -1 if loc is beyond the end of file
  if (loc > VnodeTable[fd].file_size) return -1;

  // return -1 if file is closed.
  if (VnodeTable[fd].open_status == 0) return -1;

  // Relocate the file position
  VnodeTable[fd].file_pos = loc; 

  // Return successfully
  return (0);
}
