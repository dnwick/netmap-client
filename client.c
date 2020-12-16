#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/sysctl.h>
#include <net/ethernet.h>
#include <netinet/ether.h>
#include <netinet/ip.h>
#include <netinet/udp.h>
#include <netinet/ip6.h>
#include <string.h>
#include <stdbool.h>
#include <poll.h>
#define NETMAP_WITH_LIBS
#include <net/netmap_user.h>
#include <net/netmap.h>
#include <libnetmap.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <pthread.h>
#include <sys/queue.h>
#include <semaphore.h>
#define VIRT_HDR_2	12	/* length of the extenede vnet-hdr */
#define VIRT_HDR_MAX	VIRT_HDR_2
#define MAX_BODYSIZE	65536
#define SYNC_PKT_COUNT 50000
#define FILE_SIZE 1252242504
//#define FILE_SIZE 26575110144
//#define FILE_SIZE 1000000000
int numberOfPktsToResend;
int numberOfPktsReplayed;
int allowedResendCount;
int startedSyncProcess;
volatile int curGlobalStatus;
int lastReadCheckpoint;
long curBytesWrittenSize;
int curPktSeqNum;
int ranResendFirstTime;
int globalRxReceivedCount;

pthread_mutex_t list_lock ;
pthread_mutex_t curPktSeqNum_lock ;
pthread_mutex_t global_status_lock ;
sem_t buff_access;
sem_t buff_empty;
sem_t buff_full;
sem_t process_access;
sem_t global_status_access;

#define	PKT(p, f, af)	\
    (p)->ipv4.f

struct pktMeta {
    int sequence_num;
    int req_type;
    int status;
    int size;
};

struct arguments {
    struct nmport_d *nmd;
    int *pktSeqList;
    int numOfPktsToSend;
    double start_time;
    int *resendCompleteEventsStarted;
};

struct sequence {
    int sequence_num;
};

struct missed_pkt_queue {
    struct sequence *seq;
    TAILQ_ENTRY(missed_pkt_queue) entries;
};


struct pkt_data {
    int payload_length;
    uint8_t payload[MAX_BODYSIZE];
};

struct pkt_queue {
    char *data;
    int length;
    TAILQ_ENTRY(pkt_queue) entries;
};

TAILQ_HEAD(, pkt_queue) tailq_head_pkt_queue;
TAILQ_HEAD(, missed_pkt_queue) tailq_head_missed_pkt_queue;

typedef struct cir_buffer {
    void *buf; // data
    void *buf_end; //end of data
    //size_t max_num_items; //max items in the buffer
    //size_t num_items;
    //size_t each_size; //size of each item
    long unsigned curr_free_length;
    long unsigned max_length;
    void *head;
    void *tail;
} cir_buffer;

void cir_buf_init(cir_buffer *cb, size_t max_num_items, size_t each_size) {

    cb->buf = malloc(max_num_items * each_size);
    if (cb->buf == NULL) {
        printf("Failed to initialize circular buffer \n");
    }
    cb->buf_end = (char *)cb->buf + max_num_items * each_size;
    cb->curr_free_length = max_num_items * each_size;
    cb->max_length = max_num_items * each_size;
    //cb->num_items = 0;
    //cb->each_size = each_size;
    
    cb->head = cb->buf;
    cb->tail = cb->buf;

}

void cir__buf_free(cir_buffer *cb){
    free(cb->buf);
}

void cir_buf_add_item(cir_buffer *cb, struct pkt_data *pktData, size_t length) {

    if (cb->curr_free_length == 0 && cb->curr_free_length < length) {
        printf("Failed to add item.Buffer is full \n");
    } else {
       // printf("Adding to the buff \n");
       // printf("BUF length $$$$$$$$ : %lu \n", length);
        memcpy(cb->head, (char*)pktData, length);
        cb->head = (char*)cb->head + length;
        cb->curr_free_length -= length;
        //free(pktData);
    }
    // if (cb->num_items == cb->max_num_items) {
        
    // } else {
    //     //printf("@@@@@@@@@@@@@@@@@@@@@ value is : %s \n", (char*)data);
    //     //printf("@@@@@@@@@@@@@@@@@@@@@ value is : %s \n", (char*)data);
    //     memcpy(cb->head, data, cb->each_size);
    //     cb->head = (char*)cb->head + cb->each_size;
    //     if (cb->head == cb->buf_end) {
    //         cb->head = cb->buf;
    //     }
    //     cb->num_items++;
    // }
}

int cir_buf_get_item(cir_buffer *cb, void *src) {
    int payload_length = 0;
    //printf("Trying to get item \n");
    if (cb->curr_free_length == cb->max_length) {
       // printf("Failed to get item.Buffer is empty. Waiting 5 seconds ! \n");
        //sleep(5);
    } else {

        //printf("Reading size of %lu \n", sizeof(int));
        //printf(" size of cb->tail %lu \n", cb->tail);

        struct pkt_data *pktData;
        pktData = (struct pkt_data*)cb->tail;
        //printf(" size of length %d \n", pktData->payload_length);
        //printf("  pktData : %s \n", pktData->payload);

        memcpy(src, pktData->payload, pktData->payload_length);
        cb->tail = (char*)cb->tail + sizeof(int) + pktData->payload_length;
        cb->curr_free_length += sizeof(int) + pktData->payload_length;
        payload_length = pktData->payload_length;
    }

    return payload_length;
}


enum pkt_processing_status { INITIAL, REPLAY };

enum req_type { FILE_META, FILE_CONTENT, FILE_RESEND , FILE_SYNC, FILE_SYNC_ACK, FILE_SYNC_COMPLETE, 
FILE_MISSED_SEQ_SEND_COMPLETE, FILE_RESEND_COMPLETE, FILE_PROCESS, FILE_MISSED_SEQ_MORE, FILE_MISSED_SEQ_SEND_COMPLETE_ACK, 
FILE_RESEND_COMPLETE_ACK};

const char *SERVER_IP = "192.168.1.1";
const int udp_port            = 8000;
const char *netmap_port_value = "netmap:enp0s25";


struct virt_header {
	uint8_t fields[VIRT_HDR_MAX];
};

struct pkt {
	//struct virt_header vh;
	struct ether_header eh;
	union {
		struct {
			struct ip ip;
            struct pktMeta pktMeta;
			struct udphdr udp;
			uint8_t body[MAX_BODYSIZE];	/* hardwired */
		} ipv4;
	};
} __attribute__((__packed__));

struct ip_range {
	char *name;
	union {
		struct {
			uint32_t start, end; /* same as struct in_addr */
		} ipv4;
	};
	uint16_t port0, port1;
};

struct mac_range {
	char *name;
	struct ether_addr start, end;
};

static uint16_t
wrapsum(uint32_t sum)
{
	sum = ~sum & 0xFFFF;
	return (htons(sum));
}

/* Compute the checksum of the given ip header. */
static uint32_t
checksum(const void *data, uint16_t len, uint32_t sum)
{
	const uint8_t *addr = (uint8_t const*)data;
	uint32_t i;

	/* Checksum all the pairs of bytes first... */
	for (i = 0; i < (len & ~1U); i += 2) {
		sum += (u_int16_t)ntohs(*((u_int16_t *)(addr + i)));
		if (sum > 0xFFFF)
			sum -= 0xFFFF;
	}
	/*
	 * If there's a single byte left over, checksum it, too.
	 * Network byte order is big-endian, so the remaining byte is
	 * the high byte.
	 */
	if (i < len) {
		sum += addr[i] << 8;
		if (sum > 0xFFFF)
			sum -= 0xFFFF;
	}
	return sum;
}

static int
extract_mac_range(struct mac_range *r)
{
	struct ether_addr *e;

	e = ether_aton(r->name);
	if (e == NULL) {
		D("invalid MAC address '%s'", r->name);
		return 1;
	}
	bcopy(e, &r->start, 6);
	bcopy(e, &r->end, 6);
	return 0;
}




/*
 * Global attributes
 */
struct glob_meta_info {
	struct pkt pkt;
    int pkt_size;
    int pkt_payload_size;
    struct ip_range src_ip;
	struct ip_range dst_ip;
    struct mac_range dst_mac;
	struct mac_range src_mac;
    int virt_header;
    struct nmport_d *nmd;

};

double now() {
    struct timeval timevalue;
    gettimeofday(&timevalue, NULL);
    return timevalue.tv_sec + timevalue.tv_usec / 1000000.;
}

static struct pkt* create_and_get_req_pkt(struct glob_meta_info *gmi){
    gmi->pkt_size = 300;
    gmi->pkt_payload_size = sizeof(int);
    gmi->src_ip.name = "192.168.1.103";
    gmi->src_ip.port0 = 1234;
	gmi->dst_ip.name = "192.168.1.105";
    gmi->dst_ip.port0 = 8000;
    gmi->dst_mac.name = "b4:a9:fc:78:63:b1";
    gmi->src_mac.name = "f0:de:f1:9a:16:ef";
    extract_mac_range(&gmi->src_mac);
    extract_mac_range(&gmi->dst_mac);
    uint16_t paylen;
    void *udp_ptr;
    struct udphdr udp;

    struct pkt *pktdata;
    pktdata = (struct pkt*)malloc(sizeof(struct pkt));
    struct ether_header *eh;
    struct ip ip;
    struct pktMeta *pktMeta;

    paylen = gmi->pkt_size - sizeof(*eh) - sizeof(ip) - sizeof(pktMeta);
    eh = &pktdata->eh;

    bcopy(&gmi->src_mac.start, eh->ether_shost, 6);
	bcopy(&gmi->dst_mac.start, eh->ether_dhost, 6);

    eh->ether_type = htons(ETHERTYPE_IP);
    udp_ptr = &pktdata->ipv4.udp;
    ip.ip_v = IPVERSION;
    ip.ip_hl = sizeof(ip) >> 2;
    ip.ip_id = 0;
    ip.ip_tos = IPTOS_LOWDELAY;
    ip.ip_len = htons(gmi->pkt_size - sizeof(*eh));
    ip.ip_id = 0;
    ip.ip_off = htons(IP_DF); /* Don't fragment */
    ip.ip_ttl = IPDEFTTL;
    ip.ip_p = IPPROTO_UDP;

    ip.ip_dst.s_addr = htonl(gmi->dst_ip.ipv4.start);
    ip.ip_src.s_addr = htonl(gmi->src_ip.ipv4.start);
    ip.ip_sum = wrapsum(checksum(&ip, sizeof(ip), 0));
    memcpy(&udp, udp_ptr, sizeof(udp));
    udp.uh_sport = htons(gmi->src_ip.port0);
	udp.uh_dport = htons(gmi->dst_ip.port0);
	udp.uh_ulen = htons(paylen);
    udp.uh_sum = wrapsum(
    checksum(&udp, sizeof(udp),	/* udp header */
    checksum(pktdata->ipv4.body,	/* udp payload */
    gmi->pkt_payload_size,
    checksum(&pktdata->ipv4.ip.ip_src, /* pseudo header */
    2 * sizeof(pktdata->ipv4.ip.ip_src),
    IPPROTO_UDP + (u_int32_t)ntohs(udp.uh_ulen)))));
    memcpy(&pktdata->ipv4.ip, &ip, sizeof(ip));
    pktMeta = &pktdata->ipv4.pktMeta;
    pktMeta->req_type = FILE_META;
    memcpy(udp_ptr, &udp, sizeof(udp));

    return pktdata;
}

void sendSyncCompletePkt(struct nmport_d *nmd) {
      
    struct glob_meta_info *gmi = calloc(1, sizeof(*gmi));
    struct netmap_if *nifp;
    struct netmap_ring *txring = NULL;
    nifp = nmd->nifp;

    txring = NETMAP_TXRING(nifp, nmd->first_tx_ring);
    u_int head = txring->head;
    struct netmap_slot *slot = &txring->slot[head];
    struct pkt* filePacket = create_and_get_req_pkt(gmi);
    filePacket->ipv4.pktMeta.req_type = FILE_SYNC_COMPLETE;

    u_int tosend = gmi->pkt_size;
    slot = &txring->slot[head];
    char *p = NETMAP_BUF(txring, slot->buf_idx);
    slot->flags = 0;
    struct pkt* sendPkt = (struct pkt*)p;
    sendPkt->eh = filePacket->eh;
    sendPkt->ipv4.ip = filePacket->ipv4.ip;
    sendPkt->ipv4.pktMeta = filePacket->ipv4.pktMeta;
    sendPkt->ipv4.udp = filePacket->ipv4.udp;
    slot->len = tosend;
    txring->cur = head;

    head = nm_ring_next(txring, head);
    slot->flags |= NS_REPORT;
    txring->head = txring->cur = head;
    head = txring->head;

    if (txring != NULL) {
        //printf("################################################# ioctl \n");
        ioctl(nmd->fd, NIOCTXSYNC, NULL);
    }
    while (nm_tx_pending(txring)) {
        //printf("################################################# still pending \n");
        ioctl(nmd->fd, NIOCTXSYNC, NULL);
        usleep(1); /* wait 1 tick */
    } 
    //printf("Sent FILE_SYNC_COMPLETE  \n");
}



static int receive_reqs(struct nmport_d *nmd) {

    //struct nm_desc *nmdesc;
    unsigned long long cntValue = 1;
    //unsigned long long total = 0;
    int i;
    int actualPktReceivedCount = 0;
    struct netmap_ring *rxring;
    struct netmap_if *nifp;
    // nmdesc = nm_open(netmap_port_value, NULL, 0, NULL);
    // if (nmdesc == NULL) {
    //     if (!errno) {
    //         printf("Failed to nm_open(%s): not a netmap port value\n", netmap_port_value);
    //     } else {
    //         printf("Failed to nm_open(%s): %s\n", netmap_port_value, strerror(errno));
    //     }
    //     return -1;
    // } else {
    //     printf("Started on netmap port %s \n", netmap_port_value);
    // }

    FILE *file_write_desc = fopen("/home/damith/workspace/files/output/output12.txt", "w+");
    if (!file_write_desc) {
        printf("Failed to open file \n");
        exit(1);
    }
    struct pollfd pfd = { .fd = nmd->fd, .events = POLLIN };
    nifp = nmd->nifp;

    

    //int file_write_desc = open("/home/damith/finalProj/fileRepo/output.txt", O_CREAT | O_RDWR, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);
    //

   

    //posix_fallocate(file_write_desc, 0,  1024 * 1024 * 1024 );

    static bool stopReceiving = false;

    
    //double elapsed_time;
    //unsigned long long int fileByteSize = 0;
    //unsigned long long int lower_limit = 1 * 1000LL * 1000LL * 1000LL * 25;
    //unsigned long long int upper_limit = 1 * 1024LL * 1024LL * 1024LL * 25;
    long int fileSeekPosition = 0;

    //bool isStart = false;

    i = poll(&pfd, 1, 1000);

    if (i < 0) {
        printf("poll() error: ");
       // goto quit;
    }


   // double start_time = 0;
   // double elapsed_time = 0;
    //double timeUsed = 0;
    //ssize_t bytesWritten = 0;
    long int receivedAllBytes = 0;
    //u_int n;
    //n = nm_ring_space(rxring);

   // ssize_t bytesWritten = 0, w;
   u_int ringSizeLimit = 1023;

    while (!stopReceiving) {

		if (poll(&pfd, 1, 1 * 1000) <= 0) {
			//clock_gettime(CLOCK_REALTIME_PRECISE, &targ->toc);
			//targ->toc.tv_sec -= 1; /* Subtract timeout time. */
			//goto out;
		}

		if (pfd.revents & POLLERR) {
			D("poll err");
			//goto quit;
		}

        uint64_t cur_space = 0;
        for (i = nmd->first_rx_ring; i <= nmd->last_rx_ring; i++) {

            int m;
            //D("in line 264");
			rxring = NETMAP_RXRING(nifp, i);
            if (nm_ring_empty(rxring))
				continue;
			/* compute free space in the ring */
			m = rxring->head + rxring->num_slots - rxring->tail;
			if (m >= (int) rxring->num_slots){
                m -= rxring->num_slots;
            }
				
			cur_space += m;
			// if (nm_ring_empty(rxring)){
            //     continue;
            // }

            u_int head, rx, n;
			//uint64_t b = 0;
            u_int limit = 512;

            head = rxring->head;
            n = nm_ring_space(rxring);
            //printf("n value is : %d \n ",n);
			

            if (n < limit){
                limit = n;
            }

            for (rx = 0; rx < limit; rx++) {
                
                //D("in hereeeeeee tooo ");
                struct netmap_slot *slot = &rxring->slot[head];
				char *p = NETMAP_BUF(rxring, slot->buf_idx);

				struct ether_header *ethh;
                struct ip *iph;
                struct udphdr *udph;
                struct pktMeta *pktMeta;

				ethh = (struct ether_header *)p;
    
                if (ethh->ether_type != htons(ETHERTYPE_IP)) {
                    /* Filter out non-IP traffic. */
                    //return 0;
                }
                iph = (struct ip *)(ethh + 1);
                
                if (iph->ip_p != IPPROTO_UDP) {
                    /* Filter out non-UDP traffic. */
                    //return 0;
                }
                pktMeta = (struct pktMeta *)(iph + 1);
                udph = (struct udphdr *)(pktMeta + 1);
                //printf("udph->uh_dport value is : %d", udph->uh_dport);
                head = nm_ring_next(rxring, head);
                if (udph->uh_dport == htons(8000)) {

                    char *restOfPayload = (char*)(udph + 1);
                    
                    (void)restOfPayload;

                    if (pktMeta->req_type == FILE_META) {
                        //printf("THIS IS A META Packet \n");
                    } else if (pktMeta->req_type == FILE_CONTENT){
                        
                      // printf("actual seq number is == %llu\n", cntValue);
                       actualPktReceivedCount++;
                        (void)fileSeekPosition;
                        //(void)restOfPayload;
                        receivedAllBytes += pktMeta->size;
                        
                        cntValue++;

                        ssize_t bytesWritten = write(fileno(file_write_desc),restOfPayload, pktMeta->size);
                        //(void)bytesWritten;
                        curBytesWrittenSize += bytesWritten;

                        if (curBytesWrittenSize == FILE_SIZE) {
                            printf("Done writing the file \n");
                        }

                        if (actualPktReceivedCount % ringSizeLimit == 0) {
                            rxring->head = rxring->cur = head;
                            do {
                                //rxring = NETMAP_RXRING(nifp, i);
                                n = nm_ring_space(rxring);

                                ioctl(pfd.fd, NIOCRXSYNC, NULL);

                            } while (n > 0);
                            //printf("Now n is : %d \n", n);
                            sendSyncCompletePkt(nmd);
                            break;
                        }

                        
						
                        
                        if (pktMeta->status == 1) {  
                            printf("Got first packet \n");
                            //actualPktReceivedCount = pktMeta->sequence_num;
                        }
                        if (pktMeta->status == 3) {
                            printf("Got last packet \n");
                        
                        }
                      

                       
                    }
                }
                // if (rxring != NULL) {
                //     //printf("################################################# flushing the ring \n");
                //     ioctl(nmdesc->fd, NIOCRXSYNC, NULL);
                // }

                
            }
            rxring->head = rxring->cur = head;
           // printf("actual seq number is == %llu\n", cntValue);
            // if (rxring != NULL) {
            //         //printf("################################################# flushing the ring \n");
            //     ioctl(nmdesc->fd, NIOCRXSYNC, NULL);
            // }

            // do {
            //     n = nm_ring_space(rxring);
            //     if (rxring != NULL) {
            //         //printf("################################################# flushing the ring \n");
            //         ioctl(nmdesc->fd, NIOCRXSYNC, NULL);
            //     }
            // } while (n == 0);
            //D("Running not Busyyy");
            // if (poll(&pfd, 1, 1 * 1000) <= 0 ) {
			//     //clock_gettime(CLOCK_REALTIME_PRECISE, &targ->toc);
			//     //targ->toc.tv_sec -= 1; /* Subtract timeout time. */
			//     //goto out;
		    // }

            // if (pfd.revents & POLLERR) {
            //     printf("poll err \n");
            //     //goto quit;
            // }
        };




        
        // unsigned int ring;
        // int ret;

        // pfd[0].fd     = nmdesc->fd;
        // pfd[0].events = POLLIN;

        // /* We poll with a timeout to have a chance to break the main loop if
        //  * no packets are coming. */
        // ret = poll(pfd, 1, 1000);
        // if (ret < 0) {
        //     perror("poll()");
        // } else if (ret == 0) {
        //     /* Timeout */
        //     continue;
        // }

        // for (ring = nmdesc->first_rx_ring; ring <= nmdesc->last_rx_ring; ring++) {
        //     struct netmap_ring *rxring;
        //     unsigned head, tail;
        //     int batch;

        //     rxring = NETMAP_RXRING(nmdesc->nifp, ring);
        //     head   = rxring->head;
        //     tail   = rxring->tail;
        //     batch  = tail - head;
        //     if (batch < 0) {
        //         batch += rxring->num_slots;
        //     }
        //     total += batch;
        //     for (; head != tail; head = nm_ring_next(rxring, head)) {
        //         struct netmap_slot *slot = rxring->slot + head;
        //         char *buf                = NETMAP_BUF(rxring, slot->buf_idx);

        //         struct ether_header *ethh;
        //         struct ip *iph;
        //         struct udphdr *udph;
        //         struct pktMeta *pktMeta;

        //         ethh = (struct ether_header *)buf;
    
        //         if (ethh->ether_type != htons(ETHERTYPE_IP)) {
        //             /* Filter out non-IP traffic. */
        //             //return 0;
        //         }
        //         iph = (struct ip *)(ethh + 1);
                
        //         if (iph->ip_p != IPPROTO_UDP) {
        //             /* Filter out non-UDP traffic. */
        //             //return 0;
        //         }
        //         pktMeta = (struct pktMeta *)(iph + 1);
        //         udph = (struct udphdr *)(pktMeta + 1);
                
                
        //         if (udph->uh_dport == htons(8000)) {
        //             char *restOfPayload = (char*)(udph + 1);
                    

        //             if (pktMeta->req_type == FILE_META) {
        //                 //printf("THIS IS A META Packet \n");
        //             } else if (pktMeta->req_type == FILE_CONTENT){
        //                 printf("actual seq number is == %llu\n", cntValue);
        //                 cntValue++;
        //                 //printf("THIS Packet CONTAINS FILE CONTENT\n");
        //                 printf("sequence number is == %d\n", pktMeta->sequence_num);
        //                 //printf("actual seq number is == %llu\n", cntValue);
        //                 int chaSize = strlen(restOfPayload);
        //                 (void)fileSeekPosition;

        //                 //fseek(file_write_desc, fileSeekPosition, SEEK_SET);

        //                 // if (fputs(restOfPayload, file_write_desc) < 1) {
        //                 //     printf("Error on writing ti the file\n");
        //                 // }
        //                 // fileSeekPosition += chaSize;
        //                 //cir_buf_add_item(cb, restOfPayload);
        //                 //printf("size of character arrayyyyy is : %d \n", chaSize);

        //                 //char mmapData = mmap(NULL, 4096, PROT_WRITE, MAP_SHARED, file_write_desc, strlen(restOfPayload));
        //                 // if (fwrite(restOfPayload, chaSize, 1, file_write_desc) < 1) {
        //                 //     printf("Error on writing ti the file\n");
        //                 // }
        //                 fileByteSize += chaSize;
                        
        //                 // if (fileByteSize >= lower_limit && fileByteSize < upper_limit) {
        //                 //     double end_time = now();
        //                 //     elapsed_time = end_time - start_time;
        //                 //     printf("It took %f seconds to write 25GB \n ", elapsed_time);
        //                 //     //close(file_write_desc);

        //                 // }

        //             }
        //         }
        //     }
        //     rxring->cur = rxring->head = head;
        // }

    }
    return 0;

}



int main()
{
    printf("initiating server ...");
    printf("\n");
    printf("Using server ip as %s", SERVER_IP);
    printf("\n");

    struct nmport_d *nmd = nmport_prepare("netmap:enp0s25");

    if (nmd == NULL){
		printf("something is wrong ...");
    	printf("\n");
	}

	if (nmport_open_desc(nmd) < 0){
		printf("something is wrong ...");
    	printf("\n");
	} else{
		printf("Netmap opened ...");
    	printf("\n");
	}

	D("Wait %d secs for phy reset", 2);
	sleep(2);
	D("Ready to send data through netmap");

    
    TAILQ_INIT(&tailq_head_missed_pkt_queue);

    sem_init(&global_status_access, 0, 1);
    sem_init(&process_access, 0, 1);

    if (pthread_mutex_init(&list_lock, NULL) != 0) {
        printf("mutex init failed for list_lock");
        return 1;
    }

    if (pthread_mutex_init(&global_status_lock, NULL) != 0) {
        printf("mutex init failed for global_status_lock");
        return 1;
    }

    if (pthread_mutex_init(&curPktSeqNum_lock, NULL) != 0) {
        printf("mutex init failed for list_lock");
        return 1;
    }
    
    numberOfPktsToResend = 0;
    startedSyncProcess = 0;
    lastReadCheckpoint = 1;
    curBytesWrittenSize = 0;
    curPktSeqNum = 0;
    ranResendFirstTime = 0;
    curGlobalStatus = -1;
    globalRxReceivedCount = 0;

    long chunkSize = 60 * 1024 * 1024;
    int pktPayloadSize = 1400;
    long numOfPktsToSend = 0;

    for (long i = 0; i < FILE_SIZE ; i += chunkSize) {
        long bytesToRead = chunkSize;
        if (FILE_SIZE - i < bytesToRead) {
            bytesToRead = FILE_SIZE - i;
        }
        for (int w = 0; w < bytesToRead; w += pktPayloadSize) {
            long allowedBytesToRead = pktPayloadSize;
            if (bytesToRead - w < pktPayloadSize) {
                allowedBytesToRead = bytesToRead - w;
            }
            numOfPktsToSend += allowedBytesToRead / pktPayloadSize;
            if (allowedBytesToRead % pktPayloadSize != 0) {
                numOfPktsToSend++;
            } 
        }
    }

    printf("numOfPktsToSend1 = %lu \n", numOfPktsToSend);

    //int *pktSeqList = malloc(numOfPktsToSend * sizeof(int));

    // pthread_t thread_id;
    // struct arguments args;
    // args.nmd = nmd;
    // args.pktSeqList = pktSeqList;
    // args.numOfPktsToSend = numOfPktsToSend;

    // int val = pthread_create(&thread_id, NULL, resendMissedPackets, (void *)&args);
    // if (val == -1) {
    //    printf("Unable to create the thread");
    // }
    // (void)val;

    receive_reqs(nmd);
    //pthread_join(thread_id, NULL);
    //sendSyncPkt(nmd);
}



/* end of file */