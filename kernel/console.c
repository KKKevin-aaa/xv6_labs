#include <stdarg.h>

#include "types.h"
#include "param.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "fs.h"
#include "file.h"
#include "memlayout.h"
#include "riscv.h"
#include "proc.h"
#include "defs.h"
#include "ioctl.h"
#define BACKSPACE 0x100
#define CONTROL_KEY(x) ((x) - '@')

static enum CURRENT_STATE cur_state=STATE_NORMAL;
//
// Send one character to the UART.
//
void consputc(int char_to_print) {
    if (char_to_print == BACKSPACE) {
        // Visual backspace: move back, overwrite with space, move back again
        uartputc_sync('\b');
        uartputc_sync(' ');
        uartputc_sync('\b');
    } else {
        uartputc_sync(char_to_print);
    }
}

// -------------------------------------------------------------------
// Console Buffer Structure (Fully Refactored)
// -------------------------------------------------------------------
struct {  // for struct:Intra-structure Locality
    struct spinlock lock;
    int mode;   // 0 for canonical(control by kernel's console),1 for raw(control by shell)
    int echo;   // 0(close), 1(open)
#define INPUT_BUF_SIZE 5
    char buf_data[INPUT_BUF_SIZE];
    uint reader_idx;  // [Consumer] Index where the shell reads from
    uint commit_idx;  // [Boundary] Index up to which data is "committed"
                      // (Enter pressed)
    uint editor_idx;  // [Producer] Index where the user is currently typing
    // read_idx <= commit_indx <= cursor_idx <= editor_idx
    uint cursor_idx;
} console_buf;

//
// user write()s to the console go here.
//
int console_write(int is_from_user_space, uint64 source_address, int total_bytes_to_write) {
    char temp_chunk_buf[32];
    int bytes_written_so_far = 0;
    while (bytes_written_so_far < total_bytes_to_write) {
        // Calculate how many bytes to handle in this iteration (chunk size)
        int current_chunk_size = sizeof(temp_chunk_buf);
        int bytes_remaining = total_bytes_to_write - bytes_written_so_far;
        if (current_chunk_size > bytes_remaining) current_chunk_size = bytes_remaining;
        // Copy data from user space (or kernel space) to temp buf
        if (either_copyin(temp_chunk_buf, is_from_user_space, source_address + bytes_written_so_far,
                          current_chunk_size) == -1)
            break;
        uartwrite(temp_chunk_buf, current_chunk_size);
        bytes_written_so_far += current_chunk_size;
    }
    return bytes_written_so_far;
}

//
// user read()s from the console go here.
//
int console_read(int is_to_user_space, uint64 dest_address, int max_bytes_to_read) {
    if(console_buf.mode==CONSOLE_MODE_RAW){
        int fetch_char;char char_wrapped;
        int read_bytes=0;
        acquire(&console_buf.lock);
        while(console_buf.reader_idx<console_buf.editor_idx && max_bytes_to_read>0){
            fetch_char=console_buf.buf_data[console_buf.reader_idx++ % INPUT_BUF_SIZE];
            char_wrapped=fetch_char;
            if(either_copyout(is_to_user_space, dest_address, &char_wrapped, 1)==-1) 
                break;
            dest_address++;read_bytes++;max_bytes_to_read--;
            if(fetch_char=='\n')
                break;
        }
        release(&console_buf.lock);
        return read_bytes;  //NON-block
    }
    else if(console_buf.mode==CONSOLE_MODE_CANONICAL){
        uint original_request_size = max_bytes_to_read;
        int fetched_char;
        char char_wrapper;  // Need addressable char for copyout
        acquire(&console_buf.lock);
        while (max_bytes_to_read > 0) {
            // [Block Reader]
            // Wait until interrupt handler has moved the commit boundary forward.
            while (console_buf.reader_idx == console_buf.commit_idx) {
                if (killed(myproc())) { //check if the process is killed or not
                    release(&console_buf.lock); //deadlock avoidance
                    return -1;
                }
                // Sleep waiting for new committed data
                sleep(&console_buf.reader_idx, &console_buf.lock);  //release the lock atomatically and acquire while wakeup
            }   //Wait form void wakeup(void *waitChannel)(in console_intr)

            // Fetch one character from the circular buf
            fetched_char = console_buf.buf_data[console_buf.reader_idx++ % INPUT_BUF_SIZE];

            // Handle EOF (^D)
            if (fetched_char == CONTROL_KEY('D')) {
                if (max_bytes_to_read < original_request_size) {
                    // We read some data already, so push ^D back for the next call
                    // to find.
                    console_buf.reader_idx--;
                }
                break;  // Return whatever we have read so far
            }

            // Copy the character to user destination
            char_wrapper = fetched_char;
            if (either_copyout(is_to_user_space, dest_address, &char_wrapper, 1) == -1) break;

            dest_address++;
            max_bytes_to_read--;

            // Line Buffered: Return immediately after a newline
            if (fetched_char == '\n') {
                break;
            }
        }
        release(&console_buf.lock);

        return original_request_size - max_bytes_to_read;  // Actual bytes read
    }
    else    return -1;  //Not support mode
}

//
// The console input interrupt handler.
//
void consoleintr(int input_char) {
    acquire(&console_buf.lock);
    if(console_buf.mode!=CONSOLE_MODE_RAW && console_buf.mode!=CONSOLE_MODE_CANONICAL){
        //Invalid mode, force to canonical mode
        console_buf.mode=CONSOLE_MODE_CANONICAL;
    }
    if(console_buf.mode==CONSOLE_MODE_RAW){
        if(console_buf.editor_idx-console_buf.reader_idx<INPUT_BUF_SIZE){
            console_buf.buf_data[console_buf.editor_idx % INPUT_BUF_SIZE]=input_char;
            console_buf.editor_idx++;
            console_buf.commit_idx=console_buf.editor_idx;
            wakeup(&console_buf.reader_idx);
        }
        release(&console_buf.lock);
    }
    else{
        if (input_char == '\033'){
            cur_state = STATE_ESC;
            release(&console_buf.lock);
            return;
        }
        else if (input_char == '[' && cur_state == STATE_ESC){
            cur_state = STATE_CSI;
            release(&console_buf.lock);
            return;
        }
        if (cur_state == STATE_CSI) {
            if (input_char == 'D' && console_buf.cursor_idx > console_buf.commit_idx) {
                console_buf.cursor_idx--;
                consputc('\b');
            }
            else if(input_char=='C' && console_buf.cursor_idx < console_buf.editor_idx){
                uint rewrite_idx=console_buf.cursor_idx%INPUT_BUF_SIZE;
                console_buf.cursor_idx++;
                consputc(console_buf.buf_data[rewrite_idx]);
            }
            cur_state=STATE_NORMAL; //reset
            release(&console_buf.lock);
            return;
        }
        switch (input_char) {
            case CONTROL_KEY('P'):  // Print process list, include all process statement
                procdump();         //(control+P, 0x16 in ASCII)
                break;
            case CONTROL_KEY('U'):{  // Kill line.(Control+U:0x15)
                // Delete backwards until we hit the commit boundary or a newline
                while (console_buf.editor_idx != console_buf.commit_idx &&
                    console_buf.buf_data[(console_buf.editor_idx - 1) % INPUT_BUF_SIZE] != '\n') {
                    console_buf.editor_idx--;
                    consputc(BACKSPACE);  // Bytes by bytes
                    console_buf.cursor_idx=console_buf.commit_idx;
                }
                break;
            }
            case CONTROL_KEY('H'):  // Backspace(control+H:0x08)
            case '\x7f':{            // Delete key
                if (console_buf.cursor_idx != console_buf.commit_idx) {
                    // consputc('\b');
                    uint tail_len=console_buf.editor_idx-console_buf.cursor_idx;
                    char *buf=console_buf.buf_data;
                    for(uint i=0;i<tail_len;i++){
                        uint dst_idx=(console_buf.cursor_idx+i-1)%INPUT_BUF_SIZE;
                        uint src_idx=(console_buf.cursor_idx+i)%INPUT_BUF_SIZE;
                        buf[dst_idx]=buf[src_idx];
                    }
                    console_buf.editor_idx--;console_buf.cursor_idx--;
                    //send the update "string" to output device
                    for(uint i=0;i<tail_len;i++)  consputc(buf[(i+console_buf.cursor_idx)%INPUT_BUF_SIZE]);
                    consputc(BACKSPACE);
                    for(uint i=0;i<tail_len;i++)  consputc('\b');
                }
                break;
            }
            default:{  // Normal input handling
                input_char = (input_char == '\r') ? '\n' : input_char;
                if(input_char=='\n' || input_char==CONTROL_KEY('D')){
                    //commit the data
                    consputc(input_char);
                    console_buf.buf_data[console_buf.editor_idx%INPUT_BUF_SIZE]='\n';
                    console_buf.editor_idx++;
                    console_buf.commit_idx = console_buf.editor_idx;
                    console_buf.cursor_idx = console_buf.editor_idx;
                    // Wake up the reader(for reader, it will sleep if
                    // readIdx==commitIdx)
                    wakeup(&console_buf.reader_idx);
                }
                else if(input_char!=0 || console_buf.editor_idx-console_buf.reader_idx<INPUT_BUF_SIZE){ //make sure the input char validation
                    uint num = console_buf.editor_idx - console_buf.cursor_idx;
                    for(uint i=console_buf.editor_idx;i>=console_buf.cursor_idx+1;i--){   //shift right
                        uint dst_idx=i%INPUT_BUF_SIZE;
                        uint src_idx=(i-1)%INPUT_BUF_SIZE;
                        console_buf.buf_data[dst_idx]=console_buf.buf_data[src_idx];
                    }
                    console_buf.buf_data[console_buf.cursor_idx % INPUT_BUF_SIZE]=input_char;
                    //send the update "string" to output device

                    for(uint i=0;i<num+1;i++)    
                        consputc(console_buf.buf_data[(i+console_buf.cursor_idx)%INPUT_BUF_SIZE]);
                    for(uint i=0;i<num;i++)  
                        consputc('\b');
                    console_buf.editor_idx++;console_buf.cursor_idx++;
                    if(console_buf.editor_idx - console_buf.reader_idx==INPUT_BUF_SIZE){
                        console_buf.commit_idx=console_buf.editor_idx;
                        wakeup(&console_buf.reader_idx);
                    }
                }
                break;
            }
        }
        release(&console_buf.lock);
    }

}

int console_ioctl(int requset, uint64 user_addr){
    //Use spinlock(busy-wait lock):keep checking until the lock release
    int temp_value;   //mode and echo's type both are int.
    switch(requset){
        case CONSOLE_GET_MODE:
            acquire(&console_buf.lock);
            temp_value=console_buf.mode;
            release(&console_buf.lock);
            if(copyout(myproc()->pagetable, user_addr, (char *)&temp_value, sizeof(int))<0)
                return -1;
            break;
        case CONSOLE_SET_MODE:
            if(copyin(myproc()->pagetable, (char *)&temp_value, user_addr, sizeof(console_buf.mode))<0){
                return -1;
            }
            if(temp_value!=CONSOLE_MODE_CANONICAL && temp_value!=CONSOLE_MODE_RAW){
                return -1;
            }
            acquire(&console_buf.lock);
            console_buf.mode=temp_value;
            release(&console_buf.lock);
            break;
        case CONSOLE_GET_ECHO:
            acquire(&console_buf.lock);
            temp_value=console_buf.echo;
            release(&console_buf.lock);
            if(copyout(myproc()->pagetable, user_addr, (char *)&temp_value, sizeof(int))<0)
                return -1;
            break;
        case CONSOLE_SET_ECHO:
            if(copyin(myproc()->pagetable, (char *)&temp_value, user_addr, sizeof(console_buf.mode))<0){
                return -1;
            }
            if(temp_value!=CONSOLE_ECHO_OFF && temp_value!=CONSOLE_ECHO_ON){
                return -1;
            }
            acquire(&console_buf.lock);
            console_buf.echo=temp_value;
            release(&console_buf.lock);
            break;
        case CONSOLE_DUMP_PROC:
            procdump();
            break;
        default:return -1;break;
    }
    return 0;   
}

void consoleinit(void) {
    initlock(&console_buf.lock, "cons");

    uartinit();
    //default mode and echo way
    console_buf.mode=CONSOLE_MODE_RAW;
    console_buf.echo=CONSOLE_ECHO_OFF;
    devsw[CONSOLE].read = console_read;
    devsw[CONSOLE].write = console_write;
    devsw[CONSOLE].ioctl = console_ioctl;
}