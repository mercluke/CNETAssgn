#include <cnet.h>
#include <stdlib.h>
#include <string.h>

typedef enum    { DL_DATA, DL_ACK }   FRAMEKIND;

typedef struct {
    char        data[MAX_MESSAGE_SIZE];
} MSG;

typedef struct PACKET{
    CnetAddr destaddr;
    size_t len;
    MSG msg;
} PACKET;

typedef struct FRAME{
    FRAMEKIND   kind;          /* only ever DL_DATA or DL_ACK */
    size_t      len;           /* the length of the msg field only */
    int          checksum;  /* checksum of the whole frame */
    int          seq;           /* only ever 0 or 1 */
    unsigned int link;
    PACKET      packet; /*put packet at end of struct so that for an ACK i can just chop it off...*/
} FRAME;

typedef struct listNode{
        FRAME value;
        struct listNode* next;
} listNode;

typedef struct list{
        int count;
        listNode* head;
        listNode* tail;
} list;

#define PACKET_HEADER_SIZE  (sizeof(PACKET) - sizeof(MSG))
#define PACKET_SIZE(p)      (PACKET_HEADER_SIZE + p.len)
#define FRAME_HEADER_SIZE  (sizeof(FRAME) - sizeof(PACKET))

static int route[5][5] = 
    {{0,1,2,3,4},
     {1,0,2,1,1},
     {1,2,0,1,1},
     {1,1,1,0,2},
     {1,1,1,2,0}};

EVENT_HANDLER(app_down_net);     
EVENT_HANDLER(phys_up_dll);
EVENT_HANDLER(timeouts);
EVENT_HANDLER(showstate);

static void dll_up_net(FRAME f, int link);
static void net_down_dll(PACKET p);
static void dll_down_phys(FRAME f);
static void net_up_app(PACKET p);
FRAME newFrame(PACKET* p, FRAMEKIND kind, int seq, int link);
//void print_F(FRAME f);


listNode* newNode(void);
listNode* newNode_v(FRAME);
list* newList(void);
void freeList(list* theList);
void addNode(list* theList, FRAME inVal);
FRAME removeNode(list* theList);
void freeNode(list* theList, listNode* node);
int isEmpty(list* theList);
