#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h> 
#include <fcntl.h>

// make sure this flag is accessible from signal handlers
volatile sig_atomic_t fg_only=0;

// struct that holds each part of a command
struct cmd {
    char args[512][64];
    int numargs; // number of arguments provided
    char infile[128];
    char outfile[128];
};

// if shell receives ctrl-z, then toggle the flag
void set_sigtstp(int signo) {
    if (fg_only==0) {
        fg_only=1;
        write(STDOUT_FILENO, "\nEntering foreground-only mode (& is now ignored)\n", 52);
    }
    else {
        fg_only=0;  
        write(STDOUT_FILENO, "\nExiting foreground-only mode\n", 32);
    }
}

// function to set the exit status and print if necessary
void set_status(pid_t spawnpid, int childstatus, char status[64], int bg) {
    char exitstatus[64];
    // if the process terminated normally
    if (WIFEXITED(childstatus)) {
        sprintf(exitstatus, "exit value %d\n", WEXITSTATUS(childstatus));
        if (bg==0) // only set status for foreground commands
            strcpy(status, exitstatus);
        return;
    }
    // if the process was terminated by a signal
    else if (WIFSIGNALED(childstatus)) {
        sprintf(exitstatus, "process %d terminated by signal %d\n", spawnpid, WTERMSIG(childstatus));
        printf("%s", exitstatus);
        if (bg==0)
            strcpy(status, exitstatus);
        return;
    }
}

// function that takes the input to the command line and separates out the arguments
struct cmd parse_command(char* input) {
    struct cmd c;

    int pidlen;
    char *saveptr, pidstr[16], temp[32], *expand;
    char *token = strtok_r(input, " ", &saveptr);

    // if blank line, then get input again
    if(token==NULL)
        return c;

    strcpy(c.args[0], token); // first argument is the command

    memset(c.outfile, '\0', sizeof(char)*128);
    memset(c.infile, '\0', sizeof(char)*128);
    memset(temp, '\0', sizeof(char)*32);

    int i=1;
    // tokenize the rest of the arguments
    while (token!=NULL) {
        token = strtok_r(NULL, " ", &saveptr);
        if (token==NULL)
            break; 
        strcpy(c.args[i], token);

        expand = strstr(c.args[i], "$$"); // replace any instance of $$ with pid
        if (expand!=NULL) {
            sprintf(pidstr, "%d", getpid());
            pidlen = strlen(pidstr);
            expand = strstr(c.args[i], "$$");

            if (c.args[i]==NULL)
                printf("request failed\n");
            
            strcpy(temp, expand+2); // copy the string after $$
            strcpy(expand, pidstr); // put pid in where $$ was
            expand += pidlen;
            strcpy(expand, temp);
        }
        i++;
    }
    c.numargs = i;

    // get rid of the carriage return on the last argument
    int len = strlen(c.args[c.numargs-1]);
    c.args[c.numargs-1][len-1] = '\0';

    // go through the arguments, and if there is a file redirect, get the next token for filename
    for (int i=0;i<c.numargs; i++) {
        if (strcmp(c.args[i],"<")==0)
            strcpy(c.infile,c.args[i+1]);
        else if (strcmp(c.args[i],">")==0)
            strcpy(c.outfile,c.args[i+1]);            
    }

    return c;
}

int runexec (struct cmd c, int bg, int childstatus, char status[64], int bgpids[128]) {
    int res, delc=0, bgcount=0;
    struct sigaction SIGINT_action = {0}, ignore_action = {0};

    // process the list of arguments to get rid of redirects
    for (int i=0;i<c.numargs; i++) {
        if(strcmp(c.args[i],"<")==0) {
            memset(c.args[i], '\0', sizeof(char)*64);
            memset(c.args[i+1], '\0', sizeof(char)*64);
            delc+=2; // keep track of how many arguments to delete, but don't decrease numargs yet
        }
        if(strcmp(c.args[i],">")==0) {
            memset(c.args[i], '\0', sizeof(char)*64);
            memset(c.args[i+1], '\0', sizeof(char)*64);
            delc+=2;
        }
    }
    c.numargs-=delc; // now decrease the arg counter accordingly, this is to prevent gaps in memory
    char* p_args[c.numargs+1];

    // copy the processed arguments to an array of pointers for execvp, ignoring nulled memory
    for (int i=0, j=0; i<c.numargs; i++) {
        if (c.args[i]!='\0') {
            p_args[j] = c.args[i];
            j++;
        }
    }

    // add NULL pointer to end of processed arguments
    p_args[c.numargs] = '\0';

    pid_t spawnpid = fork();
    switch(spawnpid) {
        case -1:
            perror("fork()\n");
            exit(1);
            break;
        case 0: // in child process 
            // ignore everything
            ignore_action.sa_handler = SIG_IGN;
            sigaction(SIGTSTP, &ignore_action, NULL);
            sigaction(SIGINT, &ignore_action, NULL);
            sigaction(SIGHUP, &ignore_action, NULL);
            sigaction(SIGQUIT, &ignore_action, NULL);

            // only catch sigint if is a foreground process
            if (bg==0) {
                SIGINT_action.sa_handler = SIG_DFL;
                sigfillset(&SIGINT_action.sa_mask);
                SIGINT_action.sa_flags = 0;
                sigaction(SIGINT, &SIGINT_action, NULL);
            }

            // redirect input if filename given
            if (c.infile[0] != '\0') {
                int infile = open(c.infile, O_RDONLY);
                if (infile==-1) {
                    perror("source open()\n");
                    exit(1);
                }
                res = dup2(infile, 0);
                if (res==-1) {
                    perror("source dup2()");
                    exit(2);
                }
            }
            else if (bg==1) { // if bg process redirect to /dev/null
                int devnull = open("/dev/null", O_RDONLY);
                dup2(devnull, 0);
            }

            // redirect output if filename given
            if (c.outfile[0] != '\0') {
                printf("of %s",c.outfile);
                int outfile = open(c.outfile, O_WRONLY | O_CREAT | O_TRUNC, 0644);
                if (outfile == -1) {       
                    perror("target open()"); 
                    exit(1); 
                }
                res = dup2(outfile, 1);
                if (res==-1) {
                    perror("source dup2()");
                    exit(2);
                }
            }
            else if (bg==1) { // if bg process redirect to /dev/null
                int devnull = open("/dev/null", O_WRONLY);
                dup2(devnull, 1);
            }

            execvp(p_args[0], p_args);
            perror("execvp error\n");
            exit(2);
            break;

        default: 
            // if foreground process, call waitpid and set the status when it returns
            if (bg==0) {
                spawnpid = waitpid(spawnpid, &childstatus, 0);
                set_status(spawnpid, childstatus, status, bg);
                break;
            }
            else if (bg==1) { // if background process, then add to bgpids list
                printf("background pid is %d\n", spawnpid);
                for (int i=0; i<128; i++) { // find the next available spot in bgpids list
                    if (bgpids[i]!=0)
                        bgcount++;
                }
                bgpids[bgcount] = spawnpid;
            }
    }
}

int main(int argc, char *argv[]) {
    size_t size = 2048;
    char *input = (char *)malloc(size * sizeof(char));
    char status[64];
    int cderr, bg=0, childstatus=0, childpid, bgpids[128]={0};
    struct cmd c;
    memset(status, '\0', sizeof(char)*64);
    strcpy(status, "exit status 0\n");

    struct sigaction SIGTSTP_action = {0}, ignore_action = {0};

    // set the SIGTSTP handler and set everything else to be ignored
    SIGTSTP_action.sa_handler = set_sigtstp;
    sigfillset(&SIGTSTP_action.sa_mask);
    SIGTSTP_action.sa_flags = SA_RESTART;
    sigaction(SIGTSTP, &SIGTSTP_action, NULL);

    ignore_action.sa_handler = SIG_IGN;
    sigaction(SIGINT, &ignore_action, NULL);
	sigaction(SIGHUP, &ignore_action, NULL);
	sigaction(SIGQUIT, &ignore_action, NULL);

    while (1) {
        for (int i=0; i<128; i++) { // check for bg processes and set the status if they have finished
            if (bgpids[i]!=0) {
                childpid = waitpid(bgpids[i], &childstatus, WNOHANG);
                if (childpid > 0) {
                    set_status(bgpids[i], childstatus, status, bg);
                    printf("background pid %d is done: %s", bgpids[i], status);
                    bgpids[i]=0;
                }
            }
        }

        printf(": ");
        getline(&input, &size, stdin);
        c = parse_command(input);
        if (c.args[0][0]=='#' || strlen(c.args[0])==0) // if the input is a comment or empty, reprompt
            continue;

        else if (strcmp(c.args[0],"exit")==0) // exit - just break
            break;
            
        else if (strcmp(c.args[0],"cd")==0) { // cd - change to HOME or specified directory
            if (c.numargs==1)
                cderr = chdir(getenv("HOME"));
            else
                cderr = chdir(c.args[1]);
            if (cderr!=0)
                printf("error changing directories\n");
        }

        else if (strcmp(c.args[0],"status")==0) // status - print the status
            printf("%s", status);

        else { // for any other command, call exec
            if (c.numargs !=0 && strcmp(c.args[c.numargs-1],"&")==0) { // if last argument is &, set background flag and remove &
                bg = 1;
                c.args[c.numargs-1][0] = '\0';
                c.numargs--;

                // if fg_only flag is set, then just toggle it back
                if (fg_only==1)
                    bg=0;
            }
            else bg=0;
            runexec(c,bg,childstatus,status,bgpids);
        }
    }

    free(input);
    return 0;
}
