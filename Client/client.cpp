#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <stdint.h>
#include <string>
#include <fstream>
#include <iostream>
#include <sys/time.h>
#include <signal.h>


using namespace std;

/* Data packets */
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


/* Variables */

    int expected_seq_no = 0, timeout = 4;  // Expected squence Number , Timeout
    int sock;                              // client socket
    struct sockaddr_in server;             // server address and port
    struct packet file_name_pkt;           // Data packet
    struct itimerval timer;                // struct contain the timer value and interval 
    ofstream file("client.out");           // Output

/* Variables */


// error display function
void error(const char *msg) {
    perror(msg);
    exit(0);
}

void start_timer() {
    timer.it_value.tv_sec = timeout;   // this is the period between now and the first timer interrupt , if zero the timer disabled
    timer.it_value.tv_usec = 0;            // number of nanosecond
    timer.it_interval.tv_sec = 0;          // this is the period between two successive timer interrupts
    timer.it_interval.tv_usec = 0;         // number of nanosecond

    int t = setitimer(ITIMER_REAL, &timer, NULL); // set a value of an internal time

    if(t<0)
        cout << "error setting timer" << endl;
        
    cout << "Client: timer started" << endl;
    file << "Client: timer started" << endl;
}

void stop_timer() {
    timer.it_value.tv_sec = 0;             // this is the period between now and the first timer interrupt , if zero the timer disabled
    timer.it_value.tv_usec = 0;            // number of nanosecond
    int t = setitimer(ITIMER_REAL, &timer, NULL);   // set a value of an internal time
    if(t<0)
        cout << "error setting timer" << endl;
    cout << "Client: timer stopped" << endl;
    file << "Client: timer stopped" << endl;
}


void timeout_handler(int sig_no) {
    // send packet
   
    int n = sendto(sock,(char *)&file_name_pkt, sizeof(struct packet), 0,(struct sockaddr *)&server,sizeof(struct sockaddr_in));
    
    cout << "in timer handler : file_name "<< file_name_pkt.data <<" "<< n<<endl;
    file << "in timer handler : file_name "<< file_name_pkt.data <<" "<< n<<endl;
    
    while (n  < 0) { // if there is a error in sending a packet

        error("Error send in sending packet, resending...");

        n = sendto(sock,(char *)&file_name_pkt, sizeof(struct packet), 0,(struct sockaddr *)&server,sizeof(struct sockaddr_in));
    }

    start_timer(); // start timer again
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


// Go-Back-N

int start_gbn_client() {
    int n;                                  // number of bytes
    unsigned int length;                    // len of struct sockadd_in
    struct sockaddr_in from;                // server address and port 
    struct hostent *hp;                     // struct contain info about host 
    char pkt_buf[sizeof(struct packet)];    // data
    
    string server_address;
    int server_port_num; 
    string file_name;

    ifstream input_file("client.in");
    if(!input_file.is_open())
        cout << "ERROR: can't open file\n";
    else {
        // get info about Server adderss, Server port number, file name from Client input
        string line;
        getline(input_file, server_address);
        getline(input_file, line);
        server_port_num = atoi(line.c_str());
        getline(input_file, file_name);

        // handle timer signal
        struct sigaction sig_action;
        sig_action.sa_handler = timeout_handler;
        sig_action.sa_flags = SA_RESTART;
        sigaction (SIGALRM, &sig_action, NULL);

        //client socket
        sock= socket(AF_INET, SOCK_DGRAM, 0);               
        if (sock < 0) error("socket");

        //initiate info about server
        server.sin_family = AF_INET;

        hp = gethostbyname(server_address.c_str());  // get info about host

        if (hp==0) error("Unknown host");

        // copy IP address to server.sin_addr
        bcopy((char *)hp->h_addr,
             (char *)&server.sin_addr,
              hp->h_length);

        server.sin_port = htons(server_port_num);    // set server port
        length=sizeof(struct sockaddr_in);           // get size of sockaddr_in struct

        // Packet sequance number and length
        file_name_pkt.seqno = 0;
        file_name_pkt.len = 8 + strlen(file_name.c_str());

        // set data
        memcpy(file_name_pkt.data, file_name.c_str(), strlen(file_name.c_str()));

        // send packet to server
        n = sendto(sock, (char *)&file_name_pkt, sizeof(struct packet), 0, (const struct sockaddr *)&server, length);

        start_timer();          // start timer

        if (n < 0) error("Sendto");

        ofstream output_file(file_name.c_str(), ios::binary);

        //receive packet
        n = recvfrom(sock,pkt_buf,sizeof(struct packet),0,(struct sockaddr *)&from, &length);
        stop_timer();

        while(n){

           if (n < 0) error("recvfrom");

           cout << "Client: received packet" << endl;
           file << "Client: received packet" << endl;

           struct packet *pkt = (struct packet *) pkt_buf;

            // Ending Packet
           if(pkt->len == 0) {
               cout << "Client: Ending pkt received" << endl;
               file << "Client: Ending pkt received" << endl;
               output_file.close();
               break;
           } 
           else if(pkt->len > sizeof(struct packet)) {
               cout << "Client: ERROR file not found at server" << endl;
               output_file.close();
               break;
           }

           cout << "pkt length = " << pkt->len << endl;
           cout << "pkt seqNo = " << pkt->seqno << endl;
           file << "pkt length = " << pkt->len << endl;
           file << "pkt seqNo = " << pkt->seqno << endl;

            // receive packet in order and not corrupted
           if(pkt->seqno == expected_seq_no && pkt->cksum == compute_packet_checksum(pkt)) {
               
               int data_size = pkt->len - 8;
               
               for(int i=0; i<data_size; i++) {
                   output_file << pkt->data[i];     // save data 
               }
               expected_seq_no++;
           }

           /*send ACK*/
           struct ack_packet *ack = (struct ack_packet *)malloc(sizeof(ack_packet));
           ack->ackno = expected_seq_no-1;
           ack->len = 8;
           ack->cksum = compute_ack_checksum(ack);

           n = sendto(sock, (char *)ack, 8, 0, (const struct sockaddr *)&from, length);

           if(n<0) cout << "error in sending ack\n";

           cout << "Client: ack "<< ack->ackno <<" sent\n\n";
           file << "Client: ack "<< ack->ackno <<" sent\n\n";

           // receive the rest data
           n = recvfrom(sock,pkt_buf,sizeof(struct packet),0,(struct sockaddr *)&from, &length);
       }

       file.close();
       close(sock);
   }

   return 0;
}



int start_stop_and_wait_client() {

    int sock, n;                 // number of bytes
    unsigned int length;        // len of struct sockadd_in
    struct sockaddr_in from;    // server address and port 
    struct hostent *hp;         // struct contain info about host
    char pkt_buf[512];          // data

    string server_address;
    int server_port_num;
    string file_name;

    ifstream input_file("client.in");

    if(!input_file.is_open())
        cout << "ERROR: can't open file\n";
    else {
        // get info about Server adderss, Server port number, Client port number, file name, recv window size from Client input
        string line;
        getline(input_file, server_address);
        getline(input_file, line);
        server_port_num = atoi(line.c_str());
        getline(input_file, file_name);

        //client socket
        sock= socket(AF_INET, SOCK_DGRAM, 0);
        if (sock < 0) error("Client: error in socket");
        
        //initiate info about server
        server.sin_family = AF_INET;
        hp = gethostbyname(server_address.c_str());      // get info about host
        if (hp==0) error("Client: unknown host");
        
        // copy IP address to server.sin_addr
        bcopy((char *)hp->h_addr,(char *)&server.sin_addr,hp->h_length);

        server.sin_port = htons(server_port_num);       // set server port
        length=sizeof(struct sockaddr_in);              // get size of sockaddr_in struct

        // Packet sequance number and length
        file_name_pkt.seqno = 0;
        file_name_pkt.len = 8 + strlen(file_name.c_str());

        //set data
        memcpy(file_name_pkt.data, file_name.c_str(), strlen(file_name.c_str()));

        // send packet to server
        n=sendto(sock, (char *)&file_name_pkt, sizeof(struct packet), 0, (const struct sockaddr *)&server, length);
        if (n < 0) error("Sendto");

        ofstream output_file(file_name.c_str(), ios::binary);

        // receive packet
        n = recvfrom(sock,pkt_buf,sizeof(struct packet),0,(struct sockaddr *)&from, &length);

        while(n){

           cout << "Client: received packet" << endl;
           file << "Client: received packet" << endl;

           struct packet *pkt = (struct packet *) pkt_buf;

            // Ending Packet
           if(pkt->len == 0) {
               cout << "Client: Ending pkt received" << endl;
               file << "Client: Ending pkt received" << endl;
               output_file.close();
               break;
           } 
           else if(pkt->len > sizeof(struct packet)) { // file not found
               cout << "Client: ERROR file not found at server" << endl;
               output_file.close();
               break;
           }

           cout << "pkt length = " << pkt->len << endl;
           cout << "pkt seqNo = " << pkt->seqno << endl;
           file << "pkt length = " << pkt->len << endl;
           file << "pkt seqNo = " << pkt->seqno << endl;

            // Check Error by Checksum
           if (pkt->cksum == compute_packet_checksum(pkt)) {

               int data_size = pkt->len - 8;

               for(int i=0; i<data_size; i++) {
                   output_file << pkt->data[i]; // save data 
               }

               /*send ACK*/
               struct ack_packet *ack = (struct ack_packet *)malloc(sizeof(ack_packet));

               ack->ackno = pkt->seqno;
               ack->len = 8;
               ack->cksum = compute_ack_checksum(ack);

               n = sendto(sock, (char *)ack, 8, 0, (const struct sockaddr *)&from, length);

               if(n<0) cout << "error in sending ack\n";

               cout << "Client: ack "<< ack->ackno <<" sent\n\n";
               file << "Client: ack "<< ack->ackno <<" sent\n\n";

           } 
           else {
               cout << "Client: packet corrupt, not sending ack" << endl;
               file << "Client: packet corrupt, not sending ack" << endl;
           }


           n = recvfrom(sock,pkt_buf,sizeof(struct packet),0,(struct sockaddr *)&from, &length);
      }

      output_file.close();
      file.close();
      close(sock);
   }
   return 0;
}


int main(){

   start_gbn_client();

   return 0;
   
}