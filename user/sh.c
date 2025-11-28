// Shell.

#include "kernel/types.h"
#include "user/user.h"
#include "kernel/fcntl.h"
#include "kernel/stat.h"
#include "kernel/ioctl.h"
#include <stdarg.h>
// Parsed command representation
#define EXEC 1
#define REDIR 2
#define PIPE 3
#define LIST 4
#define BACK 5

#define MAXARGS 10
#define CONTROL_KEY(x) ((x) - '@')
#define CHAR_BUF_SIZE 100
#define MAX_COMMAND 10

static char fetch_buf[CHAR_BUF_SIZE];
static char accum_buf[CHAR_BUF_SIZE];    //for accumulation buffer
enum CURRENT_STATE cur_state=STATE_NORMAL;
static int fetch_process_idx=0, fetch_read_idx=0;
static char backspace[3]="\b \b";
static char single_backspace='\b';
typedef struct command_stat{
    int editor_idx;
    char saved_buf[CHAR_BUF_SIZE];
}command_stat;
static command_stat command_buf[MAX_COMMAND];
static int history_idx=0, history_view_idx=-1;
//Base structure(all command first member is type,
//which allow us to cast into cmd structure to check its type)
struct cmd{
    int type;
};

//e.g. echo hello
struct execcmd{
    int type;
    char *argv[MAXARGS];    //parameter start pointer(pointer to existing buf)
    char *eargv[MAXARGS];   //pararmeter end pointer(pointer to existing buf in main)
};

//e.g echo hello > x
struct redircmd{
    int type;
    struct cmd *cmd;    //sub command(redirected command)
    char *file; //file name start pointer
    char *efile;    //file name end pointer
    int mode;   //open mode
    int fd; //file descriptor number
};

// e.g. ls  | wc
struct pipecmd{
    int type;
    struct cmd *left;   //left side command in pipe
    struct cmd *right;  //right side command in pipe
};

//e.g. cd a; ls
struct listcmd{
    int type;
    struct cmd *left;   //left side command in list
    struct cmd *right;  //right side command in list
};

struct backcmd{
    int type;
    struct cmd *cmd;    //command to be run in background
};

int fork1(void); // Fork but panics on failure.
void panic(char *);
struct cmd *parsecmd_main(char *);
void runcmd(struct cmd *) __attribute__((noreturn));

// Execute cmd.  Never returns.
void runcmd(struct cmd *cmd)
{
    int p[2];   //piep file descriptor array(0 for read and 1 for write)
    struct backcmd *bcmd;
    struct execcmd *ecmd;
    struct listcmd *lcmd;
    struct pipecmd *pcmd;
    struct redircmd *rcmd;

    if (cmd == 0)
        exit(1);

    switch (cmd->type)
    {
    default:
        panic("runcmd");

    case EXEC:
        ecmd = (struct execcmd *)cmd;   //cast to execcmd structure
        if (ecmd->argv[0] == 0)
            exit(1);
        exec(ecmd->argv[0], ecmd->argv);
        fprintf(2, "exec %s failed\n", ecmd->argv[0]);
        break;

    case REDIR:
        rcmd = (struct redircmd *)cmd;
        close(rcmd->fd);    //close the  file descriptor would be substituted
        if (open(rcmd->file, rcmd->mode) < 0)   //Reopen the file, which will be assigned the lowest unused fd number
        {   
            fprintf(2, "open %s failed\n", rcmd->file);
            exit(1);
        }//Here we assume the reopened fd is always the one we closed before(Unix standard)
        runcmd(rcmd->cmd);  //So now child process's 1/stdout or 0/stdin is redirected to the file
        break;

    case LIST:  //Sequential Execution
        lcmd = (struct listcmd *)cmd;
        if (fork1() == 0)   //fork one process to run the left command
            runcmd(lcmd->left);
        wait(0);    //watt for the left command to finish(Asychronous exeution)
        runcmd(lcmd->right);    //and then parent process run the right command(Tail call optimazation)
        break;

    case PIPE:
        pcmd = (struct pipecmd *)cmd;
        if (pipe(p) < 0)    //Request kernel to create a pipe, write into the p array the two file descriptors
            panic("pipe");
        if (fork1() == 0)
        {
            close(1);   //close the stardand output
            dup(p[1]);  //dup the write end of pipe to fd 1
            close(p[0]);    //Left child process: no need for read end of pipe
            close(p[1]);
            runcmd(pcmd->left);
        }
        if (fork1() == 0)
        {
            close(0);   //close the standard input
            dup(p[0]);  //dup the read end of pipe to fd 0
            close(p[0]);
            close(p[1]);    //Right child process: no need for write end of pipe
            runcmd(pcmd->right);
        }
        close(p[0]);
        close(p[1]);    //close the parent's pipe fds
        wait(0);
        wait(0);
        break;

    case BACK:
        bcmd = (struct backcmd *)cmd;
        if (fork1() == 0)
            runcmd(bcmd->cmd);
        break;  //Parent returns right away(No need wait)
    }
    exit(0);
}
void process_escape(char *buffer){ //Deal with ANSI escape sequences
    int tmp_result[1024];
    int cur_len=0;
    int logic_ptr=0, actual_ptr=0;
    while(buffer[actual_ptr]!='\n' && buffer[actual_ptr+1]!='\0'){
        if(buffer[actual_ptr]==0x1b && buffer[actual_ptr+1]=='['){
            if(buffer[actual_ptr+1]=='D' && logic_ptr>0)    logic_ptr--;
            else if(buffer[actual_ptr+1]=='C' && logic_ptr<cur_len)  logic_ptr++;
            continue;
        }
        if(cur_len<sizeof(tmp_result)-1){
            memmove(tmp_result+logic_ptr+1, tmp_result+logic_ptr, cur_len-logic_ptr);
            tmp_result[logic_ptr]=buffer[actual_ptr];
            logic_ptr++;cur_len++;
        }
        actual_ptr++;
    }
    tmp_result[logic_ptr]='\0'; //null-terminated
    memcpy((void *)buffer, (const void *)tmp_result, cur_len);
}
void aggregate_context_and_output(int element_num, ...){
    va_list args_iter;
    va_start(args_iter, element_num);
    memset(accum_buf, 0, CHAR_BUF_SIZE);
    char *temp_ptr=0;
    int length=0, have_read=0, free_size=0, count=0;//Determine if multiple repeatitions are necessary!
    int accmu_idx=0;
    memset(accum_buf, 0, CHAR_BUF_SIZE);
    for(int i=0;i<element_num;i++){
        temp_ptr=va_arg(args_iter, char *);
        length=va_arg(args_iter, int);
        count=va_arg(args_iter, int);
        while(count >0){
            while(length>0){
                //cut into smaller part if necessary
                free_size=(accmu_idx+length>CHAR_BUF_SIZE)?(CHAR_BUF_SIZE-accmu_idx):length;
                memmove(accum_buf+accmu_idx, temp_ptr+have_read, free_size);
                length-=free_size;
                have_read+=free_size;accmu_idx+=free_size;
                if(accmu_idx>=CHAR_BUF_SIZE){
                    write(1, accum_buf, CHAR_BUF_SIZE);
                    accmu_idx=0;
                }
            }
            have_read=0;free_size=0;length=0;
            count--;
        }
    }
    write(1, accum_buf, accmu_idx);
}
void save_draft_command(char *buf, int editor_idx){
    // Only save draft if we are currently at the latest line (not viewing history)
    if(history_view_idx != history_idx) return;
    if(editor_idx>CHAR_BUF_SIZE){
        fprintf(2, "Error in record current command!\n");
        exit(1);
    }
    memset(command_buf[history_view_idx%MAX_COMMAND].saved_buf, 0, CHAR_BUF_SIZE);
    memmove(command_buf[history_view_idx%MAX_COMMAND].saved_buf, buf, editor_idx);
    command_buf[history_view_idx%MAX_COMMAND].editor_idx=editor_idx;
}
void commit_cur_command(char *buf, int editor_idx){
    if(editor_idx>CHAR_BUF_SIZE){
        fprintf(2, "Error in record current command!\n");
        exit(1);
    }
    memset(command_buf[history_idx%MAX_COMMAND].saved_buf, 0, CHAR_BUF_SIZE);
    memmove(command_buf[history_idx%MAX_COMMAND].saved_buf, buf, editor_idx);
    command_buf[history_idx%MAX_COMMAND].editor_idx=editor_idx;
    history_idx++;
    history_view_idx=history_idx;   //update the view index
}
void load_prev_command(char *buf, int *editor){
    if(history_view_idx>0){
        history_view_idx--;
        *editor=command_buf[history_view_idx%MAX_COMMAND].editor_idx;
        memset(buf, 0, CHAR_BUF_SIZE);
        memmove(buf, command_buf[history_view_idx%MAX_COMMAND].saved_buf, *editor);
    }
}
void load_next_command(char *buf, int *editor){
    if(history_view_idx<history_idx){
        history_view_idx++;
        *editor=command_buf[history_view_idx%MAX_COMMAND].editor_idx;
        memset(buf, 0, CHAR_BUF_SIZE);
        memmove(buf, command_buf[history_view_idx%MAX_COMMAND].saved_buf, *editor);
    }
}
void process_char_InRawMode(char *buf, int nbuf){
    history_view_idx=history_idx;
    int editor_idx=0, cursor_idx=0, continue_flag=1, normal_tmp=0;
    int remain_size=CHAR_BUF_SIZE-fetch_read_idx;
    if(remain_size>0 && (normal_tmp=read(0, fetch_buf, remain_size))<0){
        fprintf(2, "Read error in shell!\n");
        exit(1);
    }
    fetch_read_idx+=normal_tmp;
    char fetch_char='a';    //non-zero initialization
    while(1){
        while(fetch_process_idx<fetch_read_idx&&fetch_char!='\0'){
            fetch_char=fetch_buf[fetch_process_idx++];
            if (fetch_char == '\033'){
                cur_state = STATE_ESC;continue;
            }
            else if (fetch_char == '[' && cur_state == STATE_ESC){
                cur_state = STATE_CSI;continue;
            }
            if (cur_state == STATE_CSI) {
                cur_state=STATE_NORMAL; //reset
                if (fetch_char == 'D' && cursor_idx > 0) {
                    cursor_idx--;
                    write(1, "\b", 1);
                }
                else if(fetch_char=='C' && cursor_idx < editor_idx){
                    write(1, buf+cursor_idx, 1);
                    cursor_idx++;
                }
                else if(fetch_char=='A' && history_view_idx>0){
                    if(history_idx==history_view_idx)
                        save_draft_command(buf, editor_idx);//Draft saving
                    //clear the screen
                    aggregate_context_and_output(1, backspace, 3, editor_idx);
                    load_prev_command(buf, &editor_idx);
                    cursor_idx=editor_idx;
                    aggregate_context_and_output(1, buf, editor_idx, 1);
                }
                else if(fetch_char=='B' && history_view_idx<history_idx){
                    // No need to save draft when moving down from history
                    // History is read-only, edits are discarded
                    aggregate_context_and_output(1, backspace, 3, editor_idx);
                    load_next_command(buf, &editor_idx);
                    cursor_idx=editor_idx;
                    aggregate_context_and_output(1, buf, editor_idx, 1);
                }
                continue;
            }
            switch (fetch_char) {
                case CONTROL_KEY('P'):  // Print process list, include all process statement
                    ioctl(0, CONSOLE_DUMP_PROC, 0);         //(control+P, 0x16 in ASCII)
                    break;
                case CONTROL_KEY('U'):{ // Kill line.(Control+U:0x15)
                    // Delete backwards until we hit the commit boundary or a newline
                    aggregate_context_and_output(1, backspace, 3, editor_idx);
                    editor_idx=0;cursor_idx=0;
                    break;
                }
                case CONTROL_KEY('H'): // Backspace(control+H:0x08)
                case '\x7f': {           // Delete key
                    if (cursor_idx >0 ) {
                        uint tail_len=editor_idx-cursor_idx;
                        for(uint i=0;i<tail_len;i++){
                            buf[cursor_idx+i-1]=buf[cursor_idx+i];
                        }
                        editor_idx--;cursor_idx--;
                        //send the update "string" to output device
                        //Aggregate context, output via one syscall
                        aggregate_context_and_output(3, buf+cursor_idx, tail_len, 1, backspace, 3, 1, \
                            &single_backspace, 1, tail_len);
                    }
                    break;
                }
                default:{  // Normal input handling
                    fetch_char = (fetch_char == '\r') ? '\n' : fetch_char;
                    if(fetch_char=='\n' || fetch_char==CONTROL_KEY('D')){
                        write(1, &fetch_char, 1);
                        buf[editor_idx]='\n';
                        editor_idx++;cursor_idx++;
                        continue_flag=0;
                        break;  //commit the data and quit !!!
                    }
                    else if(fetch_char!=0){ //make sure the input char validation
                        uint num = editor_idx - cursor_idx;
                        for(uint i=editor_idx;i>=cursor_idx+1;i--){   //shift right
                            buf[i]=buf[i-1];
                        }
                        buf[cursor_idx]=fetch_char;
                        //send the update "string" to output device
                        aggregate_context_and_output(2, buf+cursor_idx, num+1, 1, &single_backspace, 1, num);
                        editor_idx++;cursor_idx++;
                    }
                    break;
                }
            }
        }
        if(continue_flag==1){
            // Shift unconsumed data to the beginning to make room
            if(fetch_process_idx > 0){
                memmove(fetch_buf, fetch_buf+fetch_process_idx, fetch_read_idx-fetch_process_idx);
                fetch_read_idx -= fetch_process_idx;
                fetch_process_idx = 0;
            }
            remain_size=CHAR_BUF_SIZE-fetch_read_idx;
            if(remain_size==0)  break;
            while((normal_tmp=read(0, fetch_buf+fetch_read_idx, remain_size))==0);  //keep reading until meet newline
            fetch_read_idx+=normal_tmp;
        }
        else{   //move the unread data forward
            memmove(fetch_buf, fetch_buf+fetch_process_idx, fetch_read_idx-fetch_process_idx);
            fetch_read_idx-=fetch_process_idx;
            fetch_process_idx=0;
            commit_cur_command(buf, editor_idx);
            return;
        }
    }
    //overflow, return buffer don't end with newline
    fetch_process_idx=0;fetch_read_idx=0;   //reset the statement
    commit_cur_command(buf, CHAR_BUF_SIZE);
}

// Reads a line of input from stdin into a buffer. Prints the prompt "$ " and returns -1 if EOF (Ctrl+D) is encountered
int getcmd(char *buf, int nbuf)
{
    struct stat fd0_state;
    fstat(0, &fd0_state);
    if(fd0_state.type!=T_FILE) write(2, "$ ", 2);  //from regular file(not character device)
    memset(buf, 0, nbuf);
    int cur_mode;
    if(ioctl(0, CONSOLE_GET_MODE, (uint64)&cur_mode)<0) 
        return -1;
    if(cur_mode==CONSOLE_MODE_CANONICAL)
        gets(buf, nbuf);
    else if(cur_mode==CONSOLE_MODE_RAW)   //Process the character directly and control echoing
        process_char_InRawMode(buf, nbuf);
    else    return -1;  //Not supported mode
    if (buf[0] == 0) // EOF
        return -1;
    return 0;
}

int main(void)
{
    static char buf[100];
    int fd;

    // Ensure that three file descriptors are open.
    while ((fd = open("console", O_RDWR)) >= 0)
    {
        if (fd >= 3)
        {
            close(fd);
            break;
        }
    }

    // Read and run input commands.
    while (getcmd(buf, sizeof(buf)) >= 0)
    {
        // printf("current buf is %s\n", buf);
        char *cmd = buf;
        while (*cmd == ' ' || *cmd == '\t') cmd++;
        if (*cmd == '\n') continue; // is a blank command
        if (cmd[0] == 'c' && cmd[1] == 'd' && cmd[2] == ' ')    //Bultin command: change directory
        {
            // Chdir must be called by the parent, not the child.
            cmd[strlen(cmd) - 1] = 0; // chop \n
            if (chdir(cmd + 3) < 0) //pass the path after "cd "
                fprintf(2, "cannot cd %s\n", cmd + 3);
        }
        else
        {   //child process to run other commands
            if (fork1() == 0)
                runcmd(parsecmd_main(cmd));
            wait(0);    //Wait for the child process to finish and avoid zombie process
        }
    }
    exit(0);
}

void panic(char *s)
{
    fprintf(2, "%s\n", s);
    exit(1);
}

int fork1(void)
{
    int pid;

    pid = fork();
    if (pid == -1)
        panic("fork");
    return pid;
}

// PAGEBREAK!
//  Constructors
//Creates a basic command node (EXEC). Allocates memory and zeroes it out

struct cmd *
make_exec_cmd(void)
{
    struct execcmd *cmd;

    cmd = malloc(sizeof(*cmd));
    memset(cmd, 0, sizeof(*cmd));
    cmd->type = EXEC;
    return (struct cmd *)cmd;
}
//Creates a redirection node (REDIR). Records the filename, open mode (read/write), and the file descriptor to be replaced.
struct cmd *
make_redirections_cmd(struct cmd *subcmd, char *file, char *efile, int mode, int fd)
{
    struct redircmd *cmd;

    cmd = malloc(sizeof(*cmd));
    memset(cmd, 0, sizeof(*cmd));
    cmd->type = REDIR;
    cmd->cmd = subcmd;
    cmd->file = file;
    cmd->efile = efile;
    cmd->mode = mode;
    cmd->fd = fd;
    return (struct cmd *)cmd;
}
// Creates a pipe node (PIPE). Connects a left command and a right command
struct cmd *
make_pipe_cmd(struct cmd *left, struct cmd *right)
{
    struct pipecmd *cmd;

    cmd = malloc(sizeof(*cmd));
    memset(cmd, 0, sizeof(*cmd));
    cmd->type = PIPE;
    cmd->left = left;
    cmd->right = right;
    return (struct cmd *)cmd;
}
//Creates a list node (LIST). Handles the semicolon ;, indicating sequential execution of left and right commands
struct cmd *
make_list_cmd(struct cmd *left, struct cmd *right)
{
    struct listcmd *cmd;

    cmd = malloc(sizeof(*cmd));
    memset(cmd, 0, sizeof(*cmd));
    cmd->type = LIST;
    cmd->left = left;
    cmd->right = right;
    return (struct cmd *)cmd;
}
//Creates a background node (BACK). Handles &, indicating the sub-command runs in the background (no wait)
struct cmd *
make_back_cmd(struct cmd *subcmd)
{
    struct backcmd *cmd;

    cmd = malloc(sizeof(*cmd));
    memset(cmd, 0, sizeof(*cmd));
    cmd->type = BACK;
    cmd->cmd = subcmd;
    return (struct cmd *)cmd;
}
// PAGEBREAK!
//  Parsing

char whitespace[] = " \t\r\n\v";
char symbols[] = "<|>&;()";
// Lexer. Skips whitespace, extracts the next token (symbol or argument), and advances the parsing pointer
int gettoken(char **pointer_to_cursor, char *end_of_input, char **token_start, char **token_end)
{   //current parsiing position pointer_to_cursor, end of boundary ed; and start position of word token q, end position of word token eq
    char *cursor=*pointer_to_cursor;
    int token_type;

    while (cursor < end_of_input && strchr(whitespace, *cursor))    cursor++;
    if (token_start)
        *token_start = cursor;
    token_type = *cursor;
    switch (*cursor)
    {
        case 0: //the end character of the string
            break;
        //Signle character tokens
        case '|':
        case '(':
        case ')':
        case ';':
        case '&':
        case '<':
            cursor++;
            break;
        //Double character token
        case '>':
            cursor++;
            if (*cursor == '>'){
                token_type = '+';  //double > means append redirection
                cursor++;
            }
            break;
        //Normal word token
        default:
            token_type = 'a';  //a for argument
            while (cursor < end_of_input && !strchr(whitespace, *cursor) && !strchr(symbols, *cursor))
                cursor++;
            break;
    }
    if (token_end)
        *token_end = cursor;    //If caller need end pointer, record it!

    while (cursor < end_of_input && strchr(whitespace, *cursor))    //Skip trailing whitespace
        cursor++;
    *pointer_to_cursor = cursor;
    return token_type;
}
//Lookahead function. Skips whitespace and checks if the next character belongs to the specified set, 
//advancing the parsing pointer, returning 1 if it does and 0 otherwise
int peek(char **pointer_to_cursor, char *end_of_input, char *delimiters)
{   
    char *cursor=*pointer_to_cursor;
    while (cursor < end_of_input && strchr(whitespace, *cursor))    cursor++;
    *pointer_to_cursor = cursor;
    return *cursor && strchr(delimiters, *cursor);  //Make sure s is not at the end and current char is in delimiters
}

struct cmd *parse_line(char **, char *);
struct cmd *parse_pipe(char **, char *);
struct cmd *parse_exec(char **, char *);
struct cmd *nulterminate(struct cmd *);

struct cmd *
parsecmd_main(char *string)
{
    char *end_of_input;
    struct cmd *cmd;

    end_of_input = string + strlen(string);
    cmd = parse_line(&string, end_of_input);
    peek(&string, end_of_input, "");
    if (string != end_of_input)    //If s havn't reached the end, there must be some syntax error
    {
        fprintf(2, "leftovers: %s\n", string);
        panic("syntax");
    }
    nulterminate(cmd);  //Zero copy!
    return cmd;
}
//Deal with ; and &, construct the horizontal command tree
struct cmd *
parse_line(char **pointer_to_cursor, char *end_of_input)
{
    struct cmd *cmd;

    cmd = parse_pipe(pointer_to_cursor, end_of_input);
    while (peek(pointer_to_cursor, end_of_input, "&"))
    {
        gettoken(pointer_to_cursor, end_of_input, 0, 0);
        cmd = make_back_cmd(cmd); //Wrap the existing command into a backcmd
    }
    if (peek(pointer_to_cursor, end_of_input, ";"))
    {   //Recursively call parseline to parse the right side of the list command, and use LIST cmd type to connect them
        gettoken(pointer_to_cursor, end_of_input, 0, 0);
        cmd = make_list_cmd(cmd, parse_line(pointer_to_cursor, end_of_input));
    }
    return cmd;
}
//Deal with | operator, construct the vertical command tree
struct cmd *
parse_pipe(char **pointer_to_cursor, char *end_of_input){
    struct cmd *cmd;
    cmd = parse_exec(pointer_to_cursor, end_of_input);
    if (peek(pointer_to_cursor, end_of_input, "|")){
        gettoken(pointer_to_cursor, end_of_input, 0, 0);
        cmd = make_pipe_cmd(cmd, parse_pipe(pointer_to_cursor, end_of_input));
    }
    return cmd;
}
//Deal with redirection operators <>
struct cmd *
parse_redirections(struct cmd *cmd, char **pointer_to_cursor, char *end_of_input)
{
    int token_type;
    char *token_start, *token_end;

    while (peek(pointer_to_cursor, end_of_input, "<>"))
    {
        token_type = gettoken(pointer_to_cursor, end_of_input, 0, 0);
        if (gettoken(pointer_to_cursor, end_of_input, &token_start, &token_end) != 'a')
            panic("missing file for redirection");  //after < or > there must be a file name/Argument
        switch (token_type)
        {
        case '<'://Change the stdin to read from file(keyboard by default)
            cmd = make_redirections_cmd(cmd, token_start, token_end, O_RDONLY, 0);
            break;
        case '>'://Change the stdout to write to file(screen by default)
            cmd = make_redirections_cmd(cmd, token_start, token_end, O_WRONLY | O_CREATE | O_TRUNC, 1);
            break;
        case '+': // >>
            cmd = make_redirections_cmd(cmd, token_start, token_end, O_WRONLY | O_CREATE, 1);
            break;
        }
    }
    return cmd;
}

struct cmd *
parse_block(char **pointer_to_cursor, char *end_of_input)
{
    struct cmd *cmd;

    if (!peek(pointer_to_cursor, end_of_input, "("))
        panic("parseblock");
    gettoken(pointer_to_cursor, end_of_input, 0, 0);
    cmd = parse_line(pointer_to_cursor, end_of_input);
    if (!peek(pointer_to_cursor, end_of_input, ")"))
        panic("syntax - missing )");
    gettoken(pointer_to_cursor, end_of_input, 0, 0);
    cmd = parse_redirections(cmd, pointer_to_cursor, end_of_input);
    return cmd;
}
//Lowest level parser. Parses simple commands, parenthesized sub-commands, and redirections(Atomic commands)
struct cmd *
parse_exec(char **pointer_to_cursor, char *end_of_input)
{
    char *token_start, *token_end;
    int token_type, argc;
    struct execcmd *cmd;
    struct cmd *ret;

    if (peek(pointer_to_cursor, end_of_input, "("))
        return parse_block(pointer_to_cursor, end_of_input);

    ret = make_exec_cmd();
    cmd = (struct execcmd *)ret;    //create variable and cast to execcmd structure

    argc = 0;
    ret = parse_redirections(ret, pointer_to_cursor, end_of_input);
    while (!peek(pointer_to_cursor, end_of_input, "|)&;"))   //check if next token is not special symbol
    {
        if ((token_type = gettoken(pointer_to_cursor, end_of_input, &token_start, &token_end)) == 0)
            break;
        if (token_type != 'a')  panic("syntax");    //should be argument
        if(*token_start=='"' && *token_end=='"'){
            printf("in sh.c, we meet the \"\"\n");
            cmd->argv[argc]=token_start+1;
            cmd->argv[argc]=token_end-1;
        }
        else{
            cmd->argv[argc] = token_start;
            cmd->eargv[argc] = token_end;  //record the start and end pointer of the argument
        }
        argc++;
        if (argc >= MAXARGS)
            panic("too many args");
        ret = parse_redirections(ret, pointer_to_cursor, end_of_input); //after each argument, there may be redirection operators
    }
    cmd->argv[argc] = 0;
    cmd->eargv[argc] = 0;
    return ret;
}

// NUL-terminate all the counted strings.
struct cmd *
nulterminate(struct cmd *cmd)
{
    int i;
    struct backcmd *bcmd;
    struct execcmd *ecmd;
    struct listcmd *lcmd;
    struct pipecmd *pcmd;
    struct redircmd *rcmd;

    if (cmd == 0)
        return 0;

    switch (cmd->type)
    {
    case EXEC:
        ecmd = (struct execcmd *)cmd;
        for (i = 0; ecmd->argv[i]; i++)
            *ecmd->eargv[i] = 0;
        break;

    case REDIR:
        rcmd = (struct redircmd *)cmd;
        nulterminate(rcmd->cmd);
        *rcmd->efile = 0;
        break;

    case PIPE:
        pcmd = (struct pipecmd *)cmd;
        nulterminate(pcmd->left);
        nulterminate(pcmd->right);
        break;

    case LIST:
        lcmd = (struct listcmd *)cmd;
        nulterminate(lcmd->left);
        nulterminate(lcmd->right);
        break;

    case BACK:
        bcmd = (struct backcmd *)cmd;
        nulterminate(bcmd->cmd);
        break;
    }
    return cmd;
}
