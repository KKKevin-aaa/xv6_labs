// Shell.

#include "kernel/types.h"
#include "user/user.h"
#include "kernel/fcntl.h"
#include "kernel/stat.h"
// Parsed command representation
#define EXEC 1
#define REDIR 2
#define PIPE 3
#define LIST 4
#define BACK 5

#define MAXARGS 10

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

// Reads a line of input from stdin into a buffer. Prints the prompt "$ " and returns -1 if EOF (Ctrl+D) is encountered
int getcmd(char *buf, int nbuf)
{
    struct stat fd0_state;
    fstat(0, &fd0_state);
    if(fd0_state.type!=T_FILE) write(2, "$ ", 2);  //from regular file(not character device)
    memset(buf, 0, nbuf);
    gets(buf, nbuf);
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
