# School_work
including a proxy, shell, and a memory driver

proxy - a simple proxy with internal cache that can handle http request and return the contents.
        cache.c is the implementation of internal cache using linked list.
        
shell - implementation of a few basic shell commands with focus on properly handling various signals. 
        Commands include running executables(either on foreground or backgroudn), change jobs from 
        background to foregroud, and handling signal sent by ctrl-z & ctrl-c. 
        
cart_driver - a driver that implement unix-like IO interface with a special memory system called
              cart memory system.
