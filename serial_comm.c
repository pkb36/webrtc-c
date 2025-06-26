/* For json config */
#include <stdio.h>
#include "serial_comm.h"
#include <pthread.h>
#include <unistd.h> 
#include <termios.h> 
#include <fcntl.h>
#include <sys/select.h>
#include <string.h>
#include <stdlib.h>
#include "log_wrapper.h"
#include <json-glib/json-glib.h>
#include "device_setting.h"

extern DeviceSetting g_setting;
extern int g_move_speed;

enum DataBits
{
    DataBits5 = 5, ///< 5 data bits 5位数据位
    DataBits6 = 6, ///< 6 data bits 6位数据位
    DataBits7 = 7, ///< 7 data bits 7位数据位
    DataBits8 = 8  ///< 8 data bits 8位数据位
};

enum Parity
{
    ParityNone = 0,  ///< No Parity 无校验
    ParityOdd = 1,   ///< Odd Parity 奇校验
    ParityEven = 2,  ///< Even Parity 偶校验
    ParityMark = 3,  ///< Mark Parity 1校验
    ParitySpace = 4, ///< Space Parity 0校验
};

enum StopBits
{
    StopOne = 0,        ///< 1 stop bit 1位停止位
    StopOneAndHalf = 1, ///< 1.5 stop bit 1.5位停止位 - This is only for the Windows platform
    StopTwo = 2         ///< 2 stop bit 2位停止位
};

enum FlowControl
{
    FlowNone = 0,     ///< No flow control 无流控制
    FlowHardware = 1, ///< Hardware(RTS / CTS) flow control 硬件流控制
    FlowSoftware = 2  ///< Software(XON / XOFF) flow control 软件流控制
};


int rate2Constant(int baudrate)
{
#define B(x) \
    case x:  \
        return B##x

    switch (baudrate)
    {
#ifdef B50
        B(50);
#endif
#ifdef B75
        B(75);
#endif
#ifdef B110
        B(110);
#endif
#ifdef B134
        B(134);
#endif
#ifdef B150
        B(150);
#endif
#ifdef B200
        B(200);
#endif
#ifdef B300
        B(300);
#endif
#ifdef B600
        B(600);
#endif
#ifdef B1200
        B(1200);
#endif
#ifdef B1800
        B(1800);
#endif
#ifdef B2400
        B(2400);
#endif
#ifdef B4800
        B(4800);
#endif
#ifdef B9600
        B(9600);
#endif
#ifdef B19200
        B(19200);
#endif
#ifdef B38400
        B(38400);
#endif
#ifdef B57600
        B(57600);
#endif
#ifdef B115200
        B(115200);
#endif
#ifdef B230400
        B(230400);
#endif
#ifdef B460800
        B(460800);
#endif
#ifdef B500000
        B(500000);
#endif
#ifdef B576000
        B(576000);
#endif
#ifdef B921600
        B(921600);
#endif
#ifdef B1000000
        B(1000000);
#endif
#ifdef B1152000
        B(1152000);
#endif
#ifdef B1500000
        B(1500000);
#endif
#ifdef B2000000
        B(2000000);
#endif
#ifdef B2500000
        B(2500000);
#endif
#ifdef B3000000
        B(3000000);
#endif
#ifdef B3500000
        B(3500000);
#endif
#ifdef B4000000
        B(4000000);
#endif
        default:
            return 0;
    }
#undef B
}


int uartSet(int fd, int baudRate, char parity, char dataBits, char stopbits, char flowControl)
{
    struct termios options;

    if (tcgetattr(fd, &options) < 0)
    {
        glog_error("tcgetattr error");
        return -1;
    }

    int baudRateConstant = 0;
    baudRateConstant = rate2Constant(baudRate);

    if (0 != baudRateConstant)
    {
        cfsetispeed(&options, baudRateConstant);
        cfsetospeed(&options, baudRateConstant);
    }
    else
    {
#ifdef I_OS_LINUX
        struct termios2 tio2;

        if (-1 != ioctl(fd, TCGETS2, &tio2))
        {
            tio2.c_cflag &= ~CBAUD; // remove current baud rate
            tio2.c_cflag |= BOTHER; // allow custom baud rate using int input

            tio2.c_ispeed = baudRate; // set the input baud rate
            tio2.c_ospeed = baudRate; // set the output baud rate

            if (-1 == ioctl(fd, TCSETS2, &tio2) || -1 == ioctl(fd, TCGETS2, &tio2))
            {
                glog_error( "termios2 set custom baudrate error\n");
                return -1;
            }
        }
        else
        {
            glog_error( "termios2 ioctl error\n");
            return -1;
        }
#else
        glog_error( "not support custom baudrate\n");
        return -1;
#endif
    }

    switch (parity)
    {
        case ParityNone:
            options.c_cflag &= ~PARENB; // PARENB：产生奇偶位，执行奇偶校验
            options.c_cflag &= ~INPCK;  // INPCK：使奇偶校验起作用
            break;
        case ParityOdd:
            options.c_cflag |= PARENB; // PARENB：产生奇偶位，执行奇偶校验
            options.c_cflag |= PARODD; // PARODD：若设置则为奇校验,否则为偶校验
            options.c_cflag |= INPCK;  // INPCK：使奇偶校验起作用
            options.c_cflag |= ISTRIP; // ISTRIP：若设置则有效输入数字被剥离7个字节，否则保留全部8位
            break;
        case ParityEven:
            options.c_cflag |= PARENB;  // PARENB：产生奇偶位，执行奇偶校验
            options.c_cflag &= ~PARODD; // PARODD：若设置则为奇校验,否则为偶校验
            options.c_cflag |= INPCK;   // INPCK：使奇偶校验起作用
            options.c_cflag |= ISTRIP;  // ISTRIP：若设置则有效输入数字被剥离7个字节，否则保留全部8位
            break;
        case ParitySpace:
            options.c_cflag &= ~PARENB; // PARENB：产生奇偶位，执行奇偶校验
            options.c_cflag &= ~CSTOPB; // CSTOPB：使用两位停止位
            break;
        default:
            glog_error( "unknown parity\n");
            return -1;
    }

    switch (dataBits)
    {
        case DataBits5:
            options.c_cflag &= ~CSIZE; // 屏蔽其它标志位
            options.c_cflag |= CS5;
            break;
        case DataBits6:
            options.c_cflag &= ~CSIZE; // 屏蔽其它标志位
            options.c_cflag |= CS6;
            break;
        case DataBits7:
            options.c_cflag &= ~CSIZE; // 屏蔽其它标志位
            options.c_cflag |= CS7;
            break;
        case DataBits8:
            options.c_cflag &= ~CSIZE; // 屏蔽其它标志位
            options.c_cflag |= CS8;
            break;
        default:
            glog_error( "unknown data bits\n");
            return -1;
    }

    // 停止位
    switch (stopbits)
    {
        case StopOne:
            options.c_cflag &= ~CSTOPB; // CSTOPB：使用两位停止位
            break;
        case StopOneAndHalf:
            glog_error( "POSIX does not support 1.5 stop bits\n");
            return -1;
        case StopTwo:
            options.c_cflag |= CSTOPB; // CSTOPB：使用两位停止位
            break;
        default:
            glog_error( "unknown stop\n");
            return -1;
    }

    // 控制模式
    options.c_cflag |= CLOCAL; // 保证程序不占用串口
    options.c_cflag |= CREAD;  // 保证程序可以从串口中读取数据

    // 流控制
    switch (flowControl)
    {
        case FlowNone: ///< No flow control 无流控制
            options.c_cflag &= ~CRTSCTS;
            break;
        case FlowHardware: ///< Hardware(RTS / CTS) flow control 硬件流控制
            options.c_cflag |= CRTSCTS;
            break;
        case FlowSoftware: ///< Software(XON / XOFF) flow control 软件流控制
            options.c_cflag |= IXON | IXOFF | IXANY;
            break;
        default:
            glog_error( "unknown flow control\n");
            return -1;
    }

    // 设置输出模式为原始输出
    options.c_oflag &= ~OPOST; // OPOST：若设置则按定义的输出处理，否则所有c_oflag失效

    // 设置本地模式为原始模式
    options.c_lflag &= ~(ICANON | ECHO | ECHOE | ISIG);
    /*
     *ICANON：允许规范模式进行输入处理
     *ECHO：允许输入字符的本地回显
     *ECHOE：在接收EPASE时执行Backspace,Space,Backspace组合
     *ISIG：允许信号
     */

    options.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    
    // 设置等待时间和最小接受字符
    options.c_cc[VTIME] = 0; // 可以在select中设置
    options.c_cc[VMIN] = 1;  // 最少读取一个字符

    // 如果发生数据溢出，只接受数据，但是不进行读操作
    tcflush(fd, TCIFLUSH);

    // 激活配置
    if (tcsetattr(fd, TCSANOW, &options) < 0)
    {
        glog_error("tcsetattr failed");
        return -1;
    }

    return 0;
}


static int g_fd = -1;
static pthread_mutex_t g_mutex, g_mutex_2;
int open_serial(const char *dev_name, int baudrate)
{
    int bRet = 1;
    g_fd = open(dev_name, O_RDWR | O_NOCTTY | O_NDELAY); // 非阻塞
    if (g_fd != -1) {
        // if(fcntl(fd,F_SETFL,FNDELAY) >= 0)//非阻塞，覆盖前面open的属性
        if (fcntl(g_fd, F_SETFL, 0) >= 0){
            // set param
            if (uartSet(g_fd, baudrate, ParityNone, DataBits8, StopOne, FlowNone) == -1){
                glog_error( "uart set failed\n");
                bRet = 0;
            }
        }
        else {
            glog_error( "uart set fcntl error\n");
            bRet = 0;
        }
    } else {
        // Could not open the port
        glog_error( "open port error: Unable to open %s\n", dev_name);
        bRet = 0;
    }

    if (!bRet) {
        close_serial();
        return bRet;
    }

    pthread_mutex_init(&g_mutex,NULL);
    pthread_mutex_init(&g_mutex_2,NULL);
    return bRet;
}


void close_serial()
{
  if(g_fd != -1){
    close(g_fd);
    g_fd = -1;
  }
 
}


int is_open_serial()
{
  return (g_fd != -1);
}


int write_serial(unsigned char *data, int size)
{
  if(g_fd == -1){
    glog_error("serial device not opened before\n");
    return -1;
  }

  pthread_mutex_lock(&g_mutex);  
  int iRet = write(g_fd, data, size);
  usleep(ONE_TENTH_SEC);
//   glog_trace("write_serial: ");
//   print_serial_data(data, iRet);
  pthread_mutex_unlock(&g_mutex);
  return iRet;
}


int read_serial(unsigned char *data, int size)
{
  if(g_fd == -1){
    glog_error("serial device not opened before\n");
    return -1;
  }

  pthread_mutex_lock(&g_mutex);  
  int iRet = read(g_fd, data, size);
//   glog_trace("read_serial: ");
//   print_serial_data(data, iRet);
  pthread_mutex_unlock(&g_mutex);
  return iRet;
}


int read_serial_timeout(unsigned char *data, int size, int timeout_sec)
{
    if(g_fd == -1){
        glog_error("serial device not opened before\n");
        return -1;
    }

    struct timeval timeout;
    fd_set readfds;
    int retval;
    unsigned char buf[200] = {0};                       //LJH

    // 타임아웃 설정 (예: 5초)
    timeout.tv_sec = timeout_sec;
    timeout.tv_usec = 0;

    // 읽기 대기
    FD_ZERO(&readfds);
    FD_SET(g_fd, &readfds);

    retval = select(g_fd + 1, &readfds, NULL, NULL, &timeout);
    if (retval == -1) {
        glog_error("select error");
        return -1;
    } else if (retval) {
        int len = read(g_fd, buf, sizeof(buf));
        memcpy(data, buf, size);
        // glog_debug("read_serial: ");
        // print_serial_data(data, len);
        if(data[0] != 0x96 || len != size) {
            glog_error("set_ptz_pos read data error Sync[0x%X] Read size[%d]\n", data[0], len);
            return -1;
        }
        return len;
    } else {
        glog_error("No data within timeout \\n");
        return -1;
    }

    return -1;
}


void check_speed(unsigned char* cmd_data, int cmd_len)
{
    if (strlen(g_setting.auto_ptz_seq) > 0 && cmd_len == 21 && (cmd_data[2] == 0x01 && cmd_data[3] == 0x01)) {
        g_move_speed = cmd_data[15] + cmd_data[16] + cmd_data[17];               
        // glog_trace("set g_move_speed = %d\n", g_move_speed);
    }
    else {
        g_move_speed = 0;
    }
}


int read_cmd_timeout(unsigned char* cmd_data, int cmd_len, unsigned char* read_data, int read_len, int timeout)
{
    //1. 현재의 포지션을 가져옴 
    pthread_mutex_lock(&g_mutex_2);  

    int iRet = write_serial(cmd_data, cmd_len);
    iRet = read_serial_timeout(read_data, read_len, timeout);
    if (iRet < 0){
        glog_error("Fail read serial time out \n");
        pthread_mutex_unlock(&g_mutex_2);
        return -1;            
    }
    //2. check valid 
    if(read_data[0] != 0x96 || iRet != read_len){
        glog_error("Sync=[0x%X] is not 0x96 or Read Size=[%d] is not [%d]\n", read_data[0], iRet, read_len);
        pthread_mutex_unlock(&g_mutex_2);
        return -1;
    }
    check_speed(cmd_data, cmd_len);
    
    pthread_mutex_unlock(&g_mutex_2);

    return read_len;
}


int get_ptz_pos(unsigned char* read_data, int size)
{
    if (size < 17) {
    glog_error("read_data is too small\n");
    return -1;
    }

    glog_trace("get_ptz_pos\n");

    unsigned char cmd_data[7] = {0x96,0x00,0x06,0x01,0x01,0x01,0x9F};           //LJH, command is 0x0106 = Get All Position
    int result = read_cmd_timeout(cmd_data, 7, read_data, 17, 1);               //LJH, response data length is 11, whole response size is 17
    if(result == -1){
        glog_error("failed read ptz postion\n");
        return result;
    }

    return result;
}


int hex_str2val(char* data)
{
  int result = 0;
  int len = strlen(data);
  for( int i = 0 ; i < len ; i++){
    if(*(data+i) >= 'A'){
      *(data+i) = *(data+i) - 'A' + 10;
    } else {
      *(data+i) = *(data+i) - '0';
    }
    result += (*(data+i) & 0x0f) << (4* ((len-1) - i)) ;
  }

  return result;
}


int parse_string_to_hex(const char *input_seq, unsigned char* data, int max_data_size)
{
    char *temp = strdup(input_seq);
    char *ptr = strtok(temp, ",");
	int data_len =0;
	while (ptr != NULL){
		data[data_len++] = hex_str2val(ptr);
		ptr = strtok(NULL, ",");
        if(data_len >= max_data_size) break;
	}
    free(temp);

    return data_len;
}


void print_serial_data(unsigned char *data, int len)
{
    for(int i = 0 ; i< len ; i++){
        glog_debug("0x%02x,", data[i]);
    }
    glog_debug("\n");
}
