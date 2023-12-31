#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <netinet/in.h>
#include <linux/types.h>
#include <linux/netfilter.h>		/* for NF_ACCEPT */
#include <errno.h>
#include <string.h>

#include <libnetfilter_queue/libnetfilter_queue.h>
#define TABLE_SIZE 1000003
#define MAX_LINE_LENGTH 255

typedef struct Node {
    char url[100];
    struct Node *next;
} Node;

Node *hashTable[TABLE_SIZE] = {NULL};

unsigned int hash(char *key) {
    unsigned int sum = 0;
    while(*key) {
        sum += *key++;
    }
    return sum % TABLE_SIZE;
}

void insert(char *key) {
    unsigned int index = hash(key);
    Node *newNode = (Node*)malloc(sizeof(Node));
    
    if (!newNode) {
    printf("Memory allocation failed!\n");
    exit(1);
}

    strcpy(newNode->url, key);
    newNode->next = NULL;
    
    if (!hashTable[index]) {
        hashTable[index] = newNode;
    } else {
        Node *current = hashTable[index];
        while (current->next) {
            current = current->next;
        }
        current->next = newNode;
    }
}

char* search(char *key) {
    unsigned int index = hash(key);
    Node *current = hashTable[index];
    while (current) {
        if (strcmp(current->url, key) == 0) {
            return current->url;
        }
        current = current->next;
    }
    return NULL;
}

void readFileAndInsertToHashTable(char *filename) {
    FILE *fp = fopen(filename, "r");
    if (fp == NULL) {
        printf("Error opening the file.\n");
        return;
    }

    char line[MAX_LINE_LENGTH];
    while (fgets(line, sizeof(line), fp)) {
        //printf("Read line: %s", line);
        char *token = strtok(line, ",");  
        char *url = strtok(NULL, ",");  
        if (url) {
            char urlCopy[MAX_LINE_LENGTH];
            strcpy(urlCopy, url);
           
            urlCopy[strcspn(urlCopy, "\n")] = 0;
          

            insert(urlCopy);
        }
    }

    fclose(fp);
}



int detection = 0;
char* target = NULL;

void dump(unsigned char* buf, int size) {
	int i;
	for (i = 0; i < size; i++) {
		if (i != 0 && i % 16 == 0)
			printf("\n");
		printf("%02X ", buf[i]);
	}
	printf("\n");

	int iplen = (buf[0] & 0x0F)*4;
    	printf("IP packet Len : %d\n", iplen);
	
	int tcplen = ((buf[iplen + 12] & 0xF0) >> 4) * 4;
	printf("TCP packet Len: %d\n", tcplen);
    	
	
	char* host_header = "Host: ";
    int host_header_len = strlen(host_header);
    char url[MAX_LINE_LENGTH] = {0};
    for (i = (iplen + tcplen); i <= size - host_header_len; i++) {
        if (memcmp(buf + i, host_header, host_header_len) == 0) {
			printf("-----------------------------------------------------\n");
			printf("--------------------Found host-----------------------\n");
            int j;
            for (j = 0; j < sizeof(url) - 1 && buf[i + host_header_len + j] != '\r'; j++) {
                url[j] = buf[i + host_header_len + j];
            }
            url[j] = '\0';
            printf("Found URL: %s\n", url);
            break;
        }
    }
	

    if (strlen(url) > 0) {
        if (search(url)) {
            detection = 1;
            printf("Found in Blacklist: %s\n", url);
        }
    }

	
}

static u_int32_t print_pkt (struct nfq_data *tb)
{
	int id = 0;
	struct nfqnl_msg_packet_hdr *ph;
	struct nfqnl_msg_packet_hw *hwph;
	u_int32_t mark,ifi;
	int ret;
	unsigned char *data;

	ph = nfq_get_msg_packet_hdr(tb);
	if (ph) {
		id = ntohl(ph->packet_id);
		printf("hw_protocol=0x%04x hook=%u id=%u ",
			ntohs(ph->hw_protocol), ph->hook, id);
	}

	hwph = nfq_get_packet_hw(tb);
	if (hwph) {
		int i, hlen = ntohs(hwph->hw_addrlen);

		printf("hw_src_addr=");
		for (i = 0; i < hlen-1; i++)
			printf("%02x:", hwph->hw_addr[i]);
		printf("%02x ", hwph->hw_addr[hlen-1]);
	}

	mark = nfq_get_nfmark(tb);
	if (mark)
		printf("mark=%u ", mark);

	ifi = nfq_get_indev(tb);
	if (ifi)
		printf("indev=%u ", ifi);

	ifi = nfq_get_outdev(tb);
	if (ifi)
		printf("outdev=%u ", ifi);
	ifi = nfq_get_physindev(tb);
	if (ifi)
		printf("physindev=%u ", ifi);

	ifi = nfq_get_physoutdev(tb);
	if (ifi)
		printf("physoutdev=%u ", ifi);

	ret = nfq_get_payload(tb, &data);
	if (ret >= 0)
		printf("payload_len=%d\n", ret);
		dump(data, ret);
		printf("\n");

	fputc('\n', stdout);

	return id;
}


static int cb(struct nfq_q_handle *qh, struct nfgenmsg *nfmsg,
	      struct nfq_data *nfa, void *data)
{
	u_int32_t id = print_pkt(nfa);
	printf("entering callback\n");
	if(detection==1){
		detection=0;
		printf("I prevent target");
		return nfq_set_verdict(qh, id, NF_DROP, 0, NULL);
	}
	else{

		return nfq_set_verdict(qh, id, NF_ACCEPT, 0, NULL);
	}

}

int main(int argc, char **argv)
{
	struct nfq_handle *h;
	struct nfq_q_handle *qh;
	struct nfnl_handle *nh;
	int fd;
	int rv;
	char buf[4096] __attribute__ ((aligned));
	
	 if (argc != 2) {
        	fprintf(stderr, "Usage: %s <target_string>\n", argv[0]);
        	exit(1);
    	}

	readFileAndInsertToHashTable(argv[1]);

	printf("opening library handle\n");
	h = nfq_open();
	if (!h) {
		fprintf(stderr, "error during nfq_open()\n");
		exit(1);
	}

	printf("unbinding existing nf_queue handler for AF_INET (if any)\n");
	if (nfq_unbind_pf(h, AF_INET) < 0) {
		fprintf(stderr, "error during nfq_unbind_pf()\n");
		exit(1);
	}

	printf("binding nfnetlink_queue as nf_queue handler for AF_INET\n");
	if (nfq_bind_pf(h, AF_INET) < 0) {
		fprintf(stderr, "error during nfq_bind_pf()\n");
		exit(1);
	}

	printf("binding this socket to queue '0'\n");
	qh = nfq_create_queue(h,  0, &cb, NULL);
	if (!qh) {
		fprintf(stderr, "error during nfq_create_queue()\n");
		exit(1);
	}

	printf("setting copy_packet mode\n");
	if (nfq_set_mode(qh, NFQNL_COPY_PACKET, 0xffff) < 0) {
		fprintf(stderr, "can't set packet_copy mode\n");
		exit(1);
	}

	fd = nfq_fd(h);

	for (;;) {
		if ((rv = recv(fd, buf, sizeof(buf), 0)) >= 0) {
			printf("pkt received\n");
			nfq_handle_packet(h, buf, rv);
			continue;
		}
		/* if your application is too slow to digest the packets that
		 * are sent from kernel-space, the socket buffer that we use
		 * to enqueue packets may fill up returning ENOBUFS. Depending
		 * on your application, this error may be ignored. nfq_nlmsg_verdict_putPlease, see
		 * the doxygen documentation of this library on how to improve
		 * this situation.
		 */
		if (rv < 0 && errno == ENOBUFS) {
			printf("losing packets!\n");
			continue;
		}
		perror("recv failed");
		break;
	}

	printf("unbinding from queue 0\n");
	nfq_destroy_queue(qh);

#ifdef INSANE
	/* normally, applications SHOULD NOT issue this command, since
	 * it detaches other programs/sockets from AF_INET, too ! */
	printf("unbinding from AF_INET\n");
	nfq_unbind_pf(h, AF_INET);
#endif

	printf("closing library handle\n");
	nfq_close(h);

	exit(0);
}

