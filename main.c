#include <stdint.h>
#include <unistd.h>
#include <stdio.h>
#include <linux/joystick.h>
#include <stdint.h>
#include <unistd.h>
#include <math.h>
#include <time.h>
#include <pthread.h>
#include <errno.h>
#include <string.h>
#include <signal.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/time.h>
#include <sched.h>
#include <getopt.h>
#include <termios.h>
#include "sumd.h"
#include "convert_ps3.h"

#define TEST 1
uint8_t mode = 0;
uint8_t stop = 0;

#define JS_AXIS_MAX 32
#define JS_BUTTON_MAX 32

int16_t js_raw_axis[JS_AXIS_MAX];
uint8_t js_raw_button[JS_BUTTON_MAX];

char js_path[255] = "/dev/input/js0";
char sumd_path[255] = "/dev/ttyUSB0";
char telem1_path[255] = "/dev/rfcomm0";
char telem2_path[255] = "/dev/ttyAMA0";

#define MAX_BUF 256
int telem1_fd=0, telem2_fd=0, js_fd =0, sumd_fd=0;


void catch_signal(int sig)
{
    printf("signal: %i\n",sig);
    stop = 1;
}

void mssleep(unsigned int ms) {
  struct timespec tim;
   tim.tv_sec = ms/1000;
   tim.tv_nsec = 1000000L * (ms % 1000);
   if(nanosleep(&tim , &tim) < 0 )
   {
      printf("Nano sleep system call failed \n");
   }
}

long TimeDiff(struct timespec *ts1, struct timespec *ts2) //return difference between ts1 and ts2 in ms
{
        struct timespec ts;
        ts.tv_sec = ts1->tv_sec - ts2->tv_sec;
        ts.tv_nsec = ts1->tv_nsec - ts2->tv_nsec;
        if (ts.tv_nsec < 0) {
                ts.tv_sec--;
                ts.tv_nsec += 1000000000;
        }
        return ts.tv_sec*1000 + ts.tv_nsec/1000000;
}

void print_usage() {
    printf("Usage: rpi_base -j -a [js_dev] -b [sumd_uart] -c [telem1_uart] -d [telem2_uart] \n");
    printf("-h\thelp\n");
    printf("-j\tJoystick test\n");  
    printf("[js_dev] path to joystick dev to read RC from [defaults: %s]\n",js_path);
    printf("[sumd_uart] path to SUMD uart to write RC to [defaults: %s]\n",sumd_path);        
    printf("[telem1_uart] path to telemetry1 uart [defaults: %s]\n",telem1_path);
    printf("[telem2_uart] path to telemetry2 uart [defaults: %s]\n",telem2_path);
}

int set_defaults(int c, char **a) {
    int option;
    while ((option = getopt(c, a,"a:b:c:d:j")) != -1) {
        switch (option)  {
            case 'a': strcpy(js_path,optarg); break;
            case 'b': strcpy(sumd_path,optarg); break;
            case 'c': strcpy(telem1_path,optarg); break;
            case 'd': strcpy(telem2_path,optarg); break;
            case 'j': mode = 1; break;
            default:
                print_usage();
                return -1;
                break;
        }
    }
    return 0;
} 

void write_sumd(int fd) {
    uint8_t len;
    int16_t len1;

    uint8_t *b = sumd_get(&len);

    len1 = write(fd,b,len);

    if (len!=len1) {
        printf("Write out of sync?!\n");
        stop = 1;
    }
}

void process() { //this runs at 100Hz
    uint16_t ppm;
    uint8_t ch;
    int8_t ret;
    uint8_t i;

    for (i=0;i<JS_AXIS_MAX;i++) {
        ret = convertAxis(i,js_raw_axis[i],&ch, &ppm);
        if (ret) {
            sumd_set(ch,ppm);
        }
        
    }

    for (i=0;i<JS_BUTTON_MAX;i++) {
        ret = convertButton(i,js_raw_button[i],&ch, &ppm);
        if (ret) {
            sumd_set(ch,ppm);
        }
    }

    write_sumd(sumd_fd);
    
}


void process_jsevent(struct js_event *e) {
    if ((e->type & JS_EVENT_INIT)==JS_EVENT_INIT) return;

    if (e->type==JS_EVENT_BUTTON) {
        js_raw_button[e->number] = e->value;
    }

    if (e->type==JS_EVENT_AXIS) {
        js_raw_axis[e->number] = e->value;
        //if (e->number==0) printf("%i\n",js_raw_axis[0]);
    }

}

void process_js(int fd) {
    struct js_event js_e_buf[32]; //read max 32 events at a time
    int len,i;

    len = read (fd, js_e_buf, sizeof(js_e_buf));

    if (len<=0) {
        printf("Error reading js: [%i] [%s]\n",errno,strerror(errno));
        stop = 1;
        return;
    } 

    if (len % sizeof(struct js_event)) {
        printf("Read out of sync?!\n");
        stop = 1;
        return;
    }

    for (i=0;i<len/sizeof(struct js_event);i++) 
        process_jsevent(&js_e_buf[i]);
}

void forward_telem(int s, int t) {
    int len,len1;
    char buf[MAX_BUF];

    len = read(s,buf,MAX_BUF);
    
    if (len<=0) {
        printf("reading error [%i] [%s]\n",errno,strerror(errno));
        stop = 1;
        return;
    }
            
    len1 = write(t,buf,len);
            
    if (len!=len1) {
        printf("writing error [%i] [%s]\n",errno,strerror(errno));
        stop = 1;
        return;
    }           
}

void loop_js() {
    struct js_event js_e;

    int len;

    while (!stop) {
        len = read (js_fd, &js_e, sizeof(js_e));    
        if (len) {
            if (js_e.type==JS_EVENT_BUTTON) {
                printf("Button: %i\t\t%i\n",js_e.number,js_e.value);
            }

            if (js_e.type==JS_EVENT_AXIS) {
                printf("Axis: %i\t\t%i\n",js_e.number,js_e.value);
            }
        }       

    }
}

void loop() {
    fd_set fdlist,rlist;
    struct timeval tv;
    int ret;
    struct timespec time_now, time_prev;
    long dt_ms;

    clock_gettime(CLOCK_REALTIME, &time_now);
    clock_gettime(CLOCK_REALTIME, &time_prev);

    FD_ZERO(&fdlist);

    FD_SET(js_fd, &fdlist);

#ifndef TEST
    FD_SET(telem1_fd, &fdlist);
    FD_SET(telem2_fd, &fdlist);
#endif

    while (!stop) {
        rlist = fdlist;
        tv.tv_sec = 0; 
        tv.tv_usec = 10000; //every 10ms - SUMD SPEC 100Hz

        ret = select (FD_SETSIZE, &rlist, NULL, NULL, &tv);

        if (ret < 0) {
              perror ("select");
              stop = 1;
              return;
        } 

        clock_gettime(CLOCK_REALTIME, &time_now);
        dt_ms = TimeDiff(&time_now,&time_prev);
     
        if (dt_ms>10) {//SUMD SPEC 100Hz
            time_prev = time_now;
            process();
        }

        if (ret == 0) { //timeout
            continue; 
        }

        if (FD_ISSET(js_fd,&rlist))
            process_js(js_fd);

        if (FD_ISSET(telem1_fd,&rlist))
            forward_telem(telem1_fd,telem2_fd);

        if (FD_ISSET(telem2_fd,&rlist))
            forward_telem(telem2_fd,telem1_fd);
    }  
}

void cleanup() {
    if (js_fd) close(js_fd);
    if (sumd_fd) close(sumd_fd);
    if (telem1_fd) close(telem1_fd);
    if (telem2_fd) close(telem2_fd);    
}

int uart_open(const char *path, int flags) {
    int ret = open(path, flags);

    if (ret<0) {
        printf("open failed on %s [%i] [%s]\n",path,errno,strerror(errno));
        return ret;
    }

    struct termios options;
    tcgetattr(ret, &options);
    options.c_cflag = B115200 | CS8 | CLOCAL | CREAD;
    options.c_iflag = IGNPAR;
    options.c_oflag = 0;
    options.c_lflag = 0;
    tcflush(ret, TCIFLUSH);
    tcsetattr(ret, TCSANOW, &options);
    return ret; 
}

int main(int argc, char **argv) {
    uint8_t i;

    signal(SIGTERM, catch_signal);
    signal(SIGINT, catch_signal);

    if (set_defaults(argc,argv)) return -1;


    sumd_init();

    for (i=0;i<JS_AXIS_MAX;i++)
        js_raw_axis[i] = 0;

    for (i=0;i<JS_BUTTON_MAX;i++)
        js_raw_button[i] = 0;

    js_fd = open(js_path, O_RDONLY | O_NONBLOCK);

    if (js_fd<0) {
        printf("open failed on %s\n",js_path);
        cleanup();
        return -1;
    }   

    if (mode==0) {
        sumd_fd = uart_open(sumd_path, O_WRONLY | O_NOCTTY | O_SYNC | O_NONBLOCK);
        if (sumd_fd<0) {
            cleanup();
            return -1;
        }   

        telem1_fd = uart_open(telem1_path, O_RDWR | O_NOCTTY | O_SYNC | O_NONBLOCK);
        if (telem1_fd<0) {
            cleanup();
            return -1;
        }

        telem2_fd = uart_open(telem2_path, O_RDWR | O_NOCTTY | O_SYNC | O_NONBLOCK);
        if (telem2_fd<0) {
            cleanup();
            return -1;
        }

        loop();
    } else {
        loop_js();
    }

    cleanup();

    return 0;
}

