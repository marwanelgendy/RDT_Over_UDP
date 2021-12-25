#include <sys/types.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <string.h>
#include <netdb.h>
#include <stdio.h>
#include <stdint.h>
#include <fstream>
#include <iostream>
#include <sys/time.h>
#include <signal.h>
#include <errno.h>
#include <mutex>
#include <ctime>
#include <pthread.h>

using namespace std;

struct packet {
    /* Header */
    uint16_t cksum; /* Optional bonus part */
    uint16_t len;
    uint32_t seqno;
    /* Data */
    char data[504]; /* Not always 500 bytes, can be less */
};

/* Ack-only packets are only 8 bytes */
struct ack_packet {
    uint16_t cksum; /* Optional bonus part */
    uint16_t len;
    uint32_t ackno;
};

// common variables 
 int new_sock;    
 struct itimerval timer;      // struct contain the timer value and interval 
 int timeout = 4;
 std::ofstream output_file("server.out");   // output file
 struct sockaddr_in from;


// GBN variables
struct packet *pkts;

timer_t **timer_ids;

int MAX_WINDOW_SIZE;

int sending_window_size = 50;

int remaining_chunks;

int base=0,  next_seq_no=0;

int duplicate_ack_num=0, resend_base=0;

int last_resent=0, timeout_counter=0;

recursive_mutex rec_mutex;

int start_time, end_time;

ofstream congWin("congestionwin.txt");


// Stop and wait variables
int expected_seq_no = 0;

int dropped_pkts = 0;

struct packet curr_pkt;


socklen_t fromlen = sizeof(struct sockaddr_in);

int retries = 0;


 // common functions
 void start_timer() {
    timer.it_value.tv_sec = timeout;    // this is the period between now and the first timer interrupt , if zero the timer disabled
    timer.it_value.tv_usec = 0;         // number of nanosecond
    timer.it_interval.tv_sec = 0;       // this is the period between two successive timer interrupts
    timer.it_interval.tv_usec = 0;      // number of nanosecond

    int t = setitimer(ITIMER_REAL, &timer, NULL);       // set a value of an internal time

    if(t<0)
        cout << "error setting timer" << endl;
    
    cout << "Server: timer started" << endl;
    output_file << "Server: timer started" << endl;
}

void stop_timer() {
    timer.it_value.tv_sec = 0;      // this is the period between now and the first timer interrupt , if zero the timer disabled
    timer.it_value.tv_usec = 0;     // number of nanosecond

    int t = setitimer(ITIMER_REAL, &timer, NULL);       // set a value of an internal time

    if(t<0)
        cout << "error setting timer" << endl;

    cout << "Server: timer stopped" << endl;
    output_file << "Server: timer stopped" << endl;
}

bool lose_packet(double loss_prob){

    return ( ( rand() % 100 ) < ( loss_prob * 100 ) ) ;
    
}

// error display function
void error(const char *msg) {
    perror(msg);
    exit(0);
}


// Checksum functiions
uint16_t compute_packet_checksum(struct packet* pkt){
    uint32_t sum = 0x0;
    uint16_t*pkt_ptr = (uint16_t*)(pkt);
    uint16_t* ptr = (uint16_t*)&(pkt->data);
    for(int i=2; i<sizeof(struct packet);i+=2){
       if(i<8){
           sum += *(++pkt_ptr);
       }else{
           sum += *(ptr++);
       }
       if(sum > 0xffff){
           sum -= 0xffff;
       }
    }
    return ~(sum);
}


uint16_t compute_ack_checksum(struct ack_packet* pkt){
    uint32_t sum = 0x0;
    uint16_t*pkt_ptr = (uint16_t*)(pkt);
    for(int i=2; i<sizeof(struct ack_packet);i+=2){
        sum += *(++pkt_ptr);
        if(sum > 0xffff){
            sum -= 0xffff;
        }
    }
    return ~(sum);
}


void send_invalid_pkt(int len) {

    socklen_t fromlen = sizeof(struct sockaddr_in);

    struct packet pkt;

    pkt.len = len;

    int n = sendto(new_sock,(char *)&(pkt), sizeof(struct packet), 0,(struct sockaddr *)&from,fromlen);

    while (n  < 0) {

        error("send in sending packet, resending...");

        n = sendto(new_sock,(char *)&pkt, sizeof(struct packet), 0,(struct sockaddr *)&from,fromlen);
        
    }
}




// GBN functions

void send_pkt(int i) {

    output_file << "Server: sending packet " << i << endl;

    cout << "Server: sending packet " << i << endl;

    int index = i % MAX_WINDOW_SIZE;

    socklen_t fromlen = sizeof(struct sockaddr_in);

    // send chunk of data to client
    int n = sendto(new_sock,(char *)&(pkts[index]), sizeof(struct packet), 0,(struct sockaddr *)&from,fromlen);

    while (n  < 0) {

        error("send in sending packet, resending...");

        // send chunk of data to client
        n = sendto(new_sock,(char *)&pkts[index], sizeof(struct packet), 0,(struct sockaddr *)&from,fromlen);
    }
}

void handle_loss_event() {

    output_file << "Server: loss event occured, cnwd became: " ;
    cout << "Server: loss event occured, cnwd became: "; 

    end_time = clock();

    stop_timer();

    sending_window_size = sending_window_size/2;

    if(sending_window_size == 0)
        sending_window_size = 1;

    output_file << sending_window_size << endl;

    cout << sending_window_size << endl;

    congWin << (end_time-start_time)/double(CLOCKS_PER_SEC)*1000 << "\t" << sending_window_size << endl;

    duplicate_ack_num = 0;

    resend_base = min(base + sending_window_size, next_seq_no);

    output_file << "Server: resending all window...base = " << base << endl << endl;
    cout << "Server: resending all window...base = " << base << endl << endl;

    start_timer();
    
    for(int i=base; i<resend_base; i++) {
        send_pkt(i);
    }
}

void timeout_handler(int sig_no) {
   
    output_file << "Server: base timed out, base = " << base<< endl;

    if(sending_window_size == 1)
        timeout_counter++;

    if(timeout_counter==3)
    {
        cout << endl << "Server: child is not getting responses, it will terminate..." << endl << endl;
        exit(0);
    }
    
    handle_loss_event();
}


// initialize buffer
void init_buffers() {

    pkts = (struct packet *)malloc(MAX_WINDOW_SIZE*sizeof(struct packet));
    output_file << "Server: buffers initialized" << endl;
}

void *recv_acks(void *arg) {

    ofstream ackFile("ackfile.txt");

    /*wait for ACKs*/
    int k=0;
    char ack_buf[8];

    socklen_t fromlen = sizeof(struct sockaddr_in);

    while(remaining_chunks || (base == 0 || base != next_seq_no)) {
        // receive ACKs
        int n = recvfrom(new_sock,ack_buf,sizeof(struct ack_packet),0,(struct sockaddr *)&from,&fromlen);

        while (n < 0)  {
            if(errno != EINTR) error("recvfrom");
            errno = 0;
            // receive ACKs
            n = recvfrom(new_sock,ack_buf,sizeof(struct ack_packet),0,(struct sockaddr *)&from,&fromlen);
        }

        struct ack_packet *ack = (struct ack_packet *) ack_buf;

        if(ack->len == 8 && ack->cksum == compute_ack_checksum(ack)) {
	    
            rec_mutex.lock();

            if(ack->ackno < base) { // duplicate

                if(ack->ackno != last_resent) { //if duplicate ack was sent but for a newly lost pkt

                    output_file << "Server: incrementing duplicate acks, ackno: " << ack->ackno << endl;
                    duplicate_ack_num ++;
                }

                if(duplicate_ack_num >= 3) {

                    last_resent = ack->ackno;           //  save last ACKed packet
                    handle_loss_event();
                }
            } 
            else {
                // base was acked ==> sending window slided and increased
                sending_window_size = min(MAX_WINDOW_SIZE, sending_window_size+1);

                end_time = clock();

                congWin << (end_time-start_time)/double(CLOCKS_PER_SEC)*1000 << "\t" << sending_window_size << endl;

                if(resend_base != 0 && resend_base < next_seq_no) {
                    
                    output_file << "Server: resending the already read but not resent pkts, resend_base: " << resend_base << endl;
                    cout << "Server: resending the already read but not resent pkts, resend_base: " << resend_base << endl;
                    
                    int indx = sending_window_size - (resend_base - (base+1));
                    
                    indx = min(resend_base+indx, next_seq_no);
                    
                    for(int i=resend_base; i<indx; i++) {
                        send_pkt(i);
                    }

                    resend_base = indx;

                    output_file << "Server: resend_base became: " << resend_base << endl;
                    cout << "Server: resend_base became: " << resend_base << endl;
                }
            }

            base = ack->ackno + 1;

            if(base == next_seq_no)
                stop_timer();
            else
                start_timer();

            ackFile << " Base became: " << base << endl;
            output_file << " base became: "<<base << endl;
            ackFile << "Server: received ACK " << endl;
            cout << "Server: received ACK " << endl;
            ackFile << "ACK no. :" << ack->ackno << endl;
            cout << "ACK no. :" << ack->ackno << endl;
            ackFile << "ACK len :" << ack->len << endl;
            ackFile << "ACK chksum :" << ack->cksum << endl << endl;

            rec_mutex.unlock();
        }
    }

    ackFile << "Sending ending packet" << endl;

    send_invalid_pkt(0);

    ackFile << "Closing ACK thread" << endl << endl;
    cout << "Closing ACK thread" << endl << endl;

    ackFile.close();
    output_file.close();

    pthread_exit(NULL);
}


int start_gbn_server() {

    int sock, data_size = 504;

    struct sockaddr_in server;

    char file_name[sizeof(struct packet)];

    int port_num;

    int random_seed;

    double loss_prob;

    bool lose = false;


    ifstream inputFile("server.in");

    if(!inputFile.is_open())

        output_file << "ERROR: can't open file\n";

    else {
        // get info about Server port number ,MAX_WINDOW_SIZE, loss_prob from Client input
        string line;

        getline(inputFile, line);
        port_num = atoi(line.c_str());

        getline(inputFile, line);
        MAX_WINDOW_SIZE = atoi(line.c_str());

        getline(inputFile, line);
        random_seed = atoi(line.c_str());

        getline(inputFile, line);
        loss_prob = atof(line.c_str());

        srand(random_seed);     // Initialize random number generator

        sock = socket(AF_INET, SOCK_DGRAM, 0);  // server socket

        if (sock < 0) error("Opening socket");

        int length = sizeof(server);

        bzero(&server,length);      //erases the data n the memory

        // initialize server family, address and port
        server.sin_family=AF_INET;
        server.sin_addr.s_addr=INADDR_ANY;
        server.sin_port=htons(port_num);

        // bind the socket to server
        if (bind(sock,(struct sockaddr *)&server,length)<0)
           error("binding");

        socklen_t fromlen = sizeof(struct sockaddr_in);

        // handle timer signal
        struct sigaction sig_action;
        sig_action.sa_handler = timeout_handler;
        sigaction (SIGALRM, &sig_action, NULL);

        int dropped_pkts = 0;

        while (1) {

            char file_name_buf[sizeof(struct packet)];

            // receive file name
            int n = recvfrom(sock,file_name_buf,sizeof(struct packet),0,(struct sockaddr *)&from,&fromlen);
            if (n < 0) error("recvfrom");

            struct packet *file_name_pkt = (struct packet *)&file_name_buf;

            // get file name from packet
            memcpy(file_name, file_name_pkt->data, file_name_pkt->len - 8);

            output_file << "Server: received file name: " << file_name << endl;

            // create process
            pid_t pid;
            pid = fork();

            if (pid == -1) perror("Fork failed");

            else if (pid == 0) {   // child process

                init_buffers();     // initialize buffer

                new_sock = socket(AF_INET, SOCK_DGRAM, 0);  // UDP socket to handle file transfer to client

                if (new_sock < 0) error("Opening socket");

                FILE *input = fopen(file_name, "rb");   // open requested file

                // if file doesn't exist send invaild packet
                if (!input) {

                    send_invalid_pkt(10000);

                    error("File does not exist!");
                    _exit(0);
                }

                // read data from requested file
                char buff[data_size];
                bzero(buff, data_size);

                int length = fread(buff, 1, data_size, input);

                char buff_next[data_size];
                bzero(buff_next, data_size);

                int length_next = fread(buff_next, 1, data_size, input);

                remaining_chunks = length_next;

                // create thread
                pthread_t thread;
                pthread_create(&thread, NULL, recv_acks, NULL);

                while(length > 0) {

                   if((resend_base == next_seq_no) && next_seq_no < base+sending_window_size) {

                       struct packet pkt;

                       pkt.len = length + 8;
                       pkt.seqno = next_seq_no;

                       memcpy(pkt.data, buff, data_size);

                       pkt.cksum = compute_packet_checksum(&pkt);       // compute Checksum for packet

                       pkts[next_seq_no % MAX_WINDOW_SIZE] = pkt;

                       lose = lose_packet(loss_prob);           // loss probability

                       rec_mutex.lock();

                        // send or discard the packet depend on the loss probability
                       if(!lose){
                           send_pkt(next_seq_no);
                       } 
                       else {
                           output_file << endl << "Server: packet " << next_seq_no << " dropped.." << endl << endl;
                           cout << endl << "Server: packet " << next_seq_no << " dropped.." << endl << endl;
                           dropped_pkts++;
                       }

                       if(next_seq_no == base)
                            start_timer();

                       next_seq_no++;

                       resend_base++;

                       rec_mutex.unlock();

                       bzero(buff, data_size);

                       memcpy(buff, buff_next, length_next);        // get the next chunk of data to send it to client

                       length = length_next;

                       bzero(buff_next, data_size);

                       length_next = fread(buff_next, 1, data_size, input);     //read the next chunk of data from requested file

                       remaining_chunks = length_next;
                    }

                }
                cout << "Server: end of sending file\n";
                cout << "Server: total dropped packets: " << dropped_pkts << endl;
                pthread_join(thread, NULL);
                break;
            }
        }
    }
    return 0;
}



// Stop and Wait functions
void send_pkt_SAW(double loss_prob) {

    int n=0;

    output_file << "Server: sending packet " << curr_pkt.seqno << endl;
    cout << "Server: sending packet " << curr_pkt.seqno << endl;

    bool loss = lose_packet(loss_prob);

    if(!loss) {

        n = sendto(new_sock,(char *)&curr_pkt, sizeof(struct packet), 0,(struct sockaddr *)&from,fromlen);

        while (n < 0) {
            error("Server: send in sending packet, resending...");
            n = sendto(new_sock,(char *)&curr_pkt, sizeof(struct packet), 0,(struct sockaddr *)&from,fromlen);
        }
    }
    else {

        output_file << endl << "Server: packet " << curr_pkt.seqno << " dropped ..." << endl << endl;
        cout << endl << "Server: packet " << curr_pkt.seqno << " dropped ..." << endl << endl;

        dropped_pkts++;
    }

    /*start timer*/
    start_timer();

    /*wait for ACK*/

    char ack_buf[8];

    n = recvfrom(new_sock,ack_buf,8,0,(struct sockaddr *)&from,&fromlen);
    if (n < 0) error("recvfrom error in send_pkt() ");

    struct ack_packet *ack = (struct ack_packet *) ack_buf;

    while(ack->len == 8 && (ack->ackno != expected_seq_no || compute_ack_checksum(ack) != ack->cksum)) {

        n = recvfrom(new_sock,ack_buf,8,0,(struct sockaddr *)&from,&fromlen);

        if (n < 0) error("recvfrom error in send_pkt() ");

        ack = (struct ack_packet *) ack_buf;
    }

    ack = (struct ack_packet *) ack_buf;

    output_file << "Server: received ACK " << endl;
    cout << "Server: received ACK " << endl;

    output_file << "ACK no. :" << ack->ackno << endl;
    cout << "ACK no. :" << ack->ackno << endl;

    output_file << "ACK len :" << ack->len << endl;
    output_file << "ACK chksum :" << ack->cksum << endl << endl;

    retries = 0;

    stop_timer();
}

void retransmit(int signo) {
   
    output_file << "Server: retransmitting packet " << curr_pkt.seqno << endl << endl;
    cout << "Server: retransmitting packet " << curr_pkt.seqno << endl << endl;

    int n = sendto(new_sock,(char *)&curr_pkt, sizeof(struct packet), 0,(struct sockaddr *)&from,fromlen);

    while (n  < 0) {

        error("Server: send in sending packet, resending...");
        n = sendto(new_sock,(char *)&curr_pkt, sizeof(struct packet), 0,(struct sockaddr *)&from,fromlen);

    }

    retries++;

    if (retries > 6) {

        cout << "Server: maximum retries (6) reached, child exiting ..." << endl;
        output_file << "Server: maximum retries (6) reached, child exiting ..." << endl;

        _exit(0);
    }
    
    start_timer();
}


int start_stop_and_wait_server() {

    int sock;

    struct sockaddr_in server;

    int data_size = 504;

    char file_name[data_size];

    int port_num;

    int sending_window_size;

    int random_seed;

    double loss_prob;

    ifstream inputFile("server.in");

    if(!inputFile.is_open())
        cout << "ERROR: can't open file\n";
    else {

        string line;

        getline(inputFile, line);
        port_num = atoi(line.c_str());

        getline(inputFile, line);
        sending_window_size = atoi(line.c_str());

        getline(inputFile, line);
        random_seed = atoi(line.c_str());

        getline(inputFile, line);
        loss_prob = atof(line.c_str());

        struct sigaction sig_action;
        sig_action.sa_handler = retransmit;
        sig_action.sa_flags =  SA_RESTART;
        sigaction (SIGALRM, &sig_action, NULL);

        sock = socket(AF_INET, SOCK_DGRAM, 0);

        if (sock < 0) error("Opening socket");

        int length = sizeof(server);

        bzero(&server,length);

        server.sin_family=AF_INET;
        server.sin_addr.s_addr=INADDR_ANY;
        server.sin_port=htons(port_num);

        if (bind(sock,(struct sockaddr *)&server,length)<0)
           error("Server: binding");

        fromlen = sizeof(struct sockaddr_in);

        while (1) {

            char file_name_buf[sizeof(struct packet)];

            int n = recvfrom(sock,file_name_buf,sizeof(struct packet),0,(struct sockaddr *)&from,&fromlen);

            if (n < 0) error("recvfrom");

            struct packet *file_name_pkt = (struct packet *)&file_name_buf;

            memcpy(file_name, file_name_pkt->data, file_name_pkt->len - 8);

            output_file << "Server: received file name: " << file_name << endl;

            pid_t pid;
            pid = fork();

            if (pid == -1) perror("Server: Fork failed");

            else if (pid == 0) 
            {   // child process

               new_sock = socket(AF_INET, SOCK_DGRAM, 0);

               if (new_sock < 0) error("Server: Opening socket");

               FILE *input = fopen(file_name, "rb");

               if (!input) {
                   send_invalid_pkt(10000);
                   error("File does not exist!");
                   _exit(0);
               }

               char buff[data_size];
               bzero(buff, data_size);

               int length = fread(buff, 1, data_size, input);

               while(length) {

                   curr_pkt.len = 8+length;
                   curr_pkt.seqno = expected_seq_no;

                   memcpy(curr_pkt.data,buff,data_size);

                   curr_pkt.cksum = compute_packet_checksum(&curr_pkt);

                   /*send and wait for ACK*/
                   send_pkt_SAW(loss_prob);
                   expected_seq_no = (++expected_seq_no) % 2;

                   bzero(buff, data_size);
                   length = fread(buff, 1, data_size, input);
                }

                send_invalid_pkt(0);

                fclose(input);

                cout << "Server: end of sending file\n";
                cout << "Server: total dropped packets: " << dropped_pkts << endl;
                output_file << "Server: end of sending file\n";
                output_file << "Server: total dropped packets: " << dropped_pkts << endl;

                break;
            }
        }
        close(sock);
    }
    return 0;
}




int main(int argc, char *argv[]) {
    start_gbn_server();
    return 0;
}