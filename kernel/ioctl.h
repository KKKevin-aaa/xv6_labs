#define CONSOLE_GET_MODE 0x1    // Example ioctl request to get console mode
#define CONSOLE_SET_MODE 0x2    // Example ioctl request to set console mode
#define CONSOLE_GET_ECHO 0x3    // Example ioctl request to clear the console screen
#define CONSOLE_SET_ECHO 0x4  // Example ioctl request to set cursor position

//Mode value
#define CONSOLE_MODE_CANONICAL 0
#define CONSOLE_MODE_RAW 1

// echo value
#define CONSOLE_ECHO_OFF 0
#define CONSOLE_ECHO_ON 1

//auxiuaily
enum CURRENT_STATE { STATE_NORMAL, STATE_ESC, STATE_CSI };