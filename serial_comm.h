#ifndef __SERIAL_COMM_H__
#define __SERIAL_COMM_H__

int open_serial(const char *dev_name, int baudrate);
int write_serial(unsigned char *data, int size);
void close_serial();
int is_open_serial();
int hex_str2val(char* data);
int parse_string_to_hex(const char *input_seq, unsigned char* data, int max_data_size);
int read_cmd_timeout(unsigned char* cmd_data, int cmd_len, unsigned char* read_data, int read_len, int timeout);
void print_serial_data(unsigned char *data, int len);

#define ONE_TENTH_SEC   (1000*100)
#define ONE_SEC         (1000*1000)

#endif
