#include "assignment.h"
#include <cnet.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

int* incrementOnFrame;
int* expectedACK;
int* expectedFrame;
list* frameQ;
CnetTimerID* lasttimer;


//main()
EVENT_HANDLER(reboot_node)
{
    frameQ = newList(); //Queue of frames to send - front of queue has already been sent and is awaiting an ACK
    incrementOnFrame = calloc((nodeinfo.nlinks+1) , sizeof(int)); //Sequence to send (one for each link)
    expectedACK    = calloc(nodeinfo.nlinks+1, sizeof(int)); //expected sequence from ack (one for each link)
    expectedFrame   = calloc(nodeinfo.nlinks+1, sizeof(int)); //expected sequence from received frame (one for each link)
    lasttimer       = calloc(nodeinfo.nlinks+1, sizeof(CnetTimerID)); //timer for last DL_DATA sent on each link

    CHECK(CNET_set_handler( EV_APPLICATIONREADY, app_down_net, 0)); //app_down_net()
    CHECK(CNET_set_handler( EV_PHYSICALREADY,    phys_up_dll, 0)); //phys_up_dll()
    CHECK(CNET_set_handler( EV_TIMER1,           timeouts, 0)); //handle timeouts
    CHECK(CNET_set_handler( EV_DEBUG0,           showstate, 0)); //button in gui to print current state of sequences

    CHECK(CNET_set_debug_string( EV_DEBUG0, "State")); //label for aforementioned button

    CNET_enable_application(ALLNODES); //allow all nodes to generate messages to

}

//Application layer generates a message to send to Network
EVENT_HANDLER(app_down_net)
{
    //turn sending LED on
    CNET_set_LED(0, "green");

    CnetAddr destaddr;
    MSG msg;
    size_t len = sizeof(MSG);
  
    //Read message from appliction
    CHECK(CNET_read_application(&destaddr, &msg, &len));

    //stop other nodes generating messages
    CNET_disable_application(ALLNODES);

    //wrap message in a packet
    PACKET p;
    p.destaddr = destaddr;
    p.len = len;
    memcpy(&p.msg, &msg, len);

    net_down_dll(p);
}


//Network layer receives message (indirectly) 
//App->Presentation->Session->Transport->Network
//and sends down to DLL
static void net_down_dll(PACKET p) 
{
    //use routing table to find link to take
    int link = route[nodeinfo.nodenumber][p.destaddr];

    FRAME f = newFrame(&p, DL_DATA, incrementOnFrame[link], link);

    printf("Message generated for node %d seq=%d\n", p.destaddr, incrementOnFrame[link]);

    incrementOnFrame[link] = 1-incrementOnFrame[link];

    //add frame to queue of "to be sent" frames
    addNode(frameQ, f);

    //if queue was empty beforehand, send frame down to datalink
    if(frameQ->count == 1)
    {
        dll_down_phys(f);
    }

}

//Datalink Layer down to Physical
static void dll_down_phys(FRAME f)
{
    //only send header if ACK, else send entire frame
    size_t len = (f.kind == DL_DATA) ? sizeof(FRAME) : FRAME_HEADER_SIZE;

    switch (f.kind) {
    case DL_ACK :
        printf("ACK transmitted, seq=%d link=%d\n", f.seq, f.link);
        break;

    case DL_DATA: {
        CnetTime        timeout;

        printf("DATA transmitted, seq=%d link=%d\n", f.seq, f.link);

        //why did the STOPANDWAIT example use this magic number?
        //what is the significance of 8000000?
        timeout = len*((CnetTime)8000000 / linkinfo[f.link].bandwidth) +
                                linkinfo[f.link].propagationdelay;

        //start timer at magic number
        lasttimer[f.link] = CNET_start_timer(EV_TIMER1, 3 * timeout, 0);

        break;
      }
    }

    //send frame down to physical and turn sending LED off
    CHECK(CNET_write_physical((unsigned int)f.link, &f, &len)); 
    CNET_set_LED(0, CNET_LED_OFF);
}



//Physical reads data and sends up to Datalink
EVENT_HANDLER(phys_up_dll)
{
    //turn receiving LED on
    CNET_set_LED(5, "red");

    FRAME   f;
    size_t  len;
    int link;

    len = sizeof(FRAME);

    //read frame from physical
    CHECK(CNET_read_physical(&link, &f, &len));

    dll_up_net(f, link);
}


//Datalink decides whether to send to network or not
static void dll_up_net(FRAME f, int link)
{
    int checksum = f.checksum;
    f.checksum = 0;


    //ensure frame is not corrupt
    if(CNET_ccitt((unsigned char *)&f, (f.kind == DL_DATA) ? sizeof(FRAME) : FRAME_HEADER_SIZE) != checksum)
    {
        printf("\t\t\t\t\tBAD checksum - frame ignored\n");
        return;
    }

    //check if frame is ACK or Data
    switch(f.kind)
    {
        case DL_ACK :
            //make sure it's the correct ACK
            if(f.seq == expectedACK[link])
            {
                printf("\t\t\t\t\tACK received on link %d, seq=%d\n", link, f.seq);
                //stop timer for related frame
                CNET_stop_timer(lasttimer[link]);
                expectedACK[link] = 1-expectedACK[link];

                //let nodes generate messages, again
                CNET_enable_application(ALLNODES);

                //Ideally, it should never be empty at this stage but I used this to debug...
                if(!isEmpty(frameQ))
                {
                    //ACK received, good to pop sent frame off of the queue
                    removeNode(frameQ);

                    //any more frames to send?
                    if(!isEmpty(frameQ))
                    {
                        dll_down_phys(frameQ->head->value);
                    }
                }
                //This is bad, this should NEVER occur
                else
                {
                    printf("\t\t\t\t\tQueue is empty: something is VERY wrong\n");
                }
            }
            else
            {
                //received incorrect ACK, ignore/discard it
                printf("\t\t\t\t\tIncorrect ACK received on link %i, seq %i vs %i\n", link, f.seq, expectedACK[link]);
            }
            break;
        case DL_DATA :

            printf("\t\t\t\t\tDATA received on link %d, seq=%d \n", link, f.seq);
            //is this the data we were expecting?
            if(f.seq == expectedFrame[link])
            {
                //Good, send an ACK and then send the packet up to network
                FRAME ack = newFrame(NULL, DL_ACK, f.seq, link);
                dll_down_phys(ack);

                expectedFrame[link] = 1-expectedFrame[link];
                
                net_up_app(f.packet);

            }
            else
            {
                //Duplicate data, ACK must have been corrupted/dropped causing a timeout
                printf("\t\t\t\t\tIgnored: expected seq=%i on link %i, received seq=%i\n", expectedFrame[link], link, f.seq);
                FRAME ack = newFrame(NULL, DL_ACK, f.seq, link);

                dll_down_phys(ack);
            }
            break;
    }
}

static void net_up_app(PACKET p)
{
    //destination node
    if(nodeinfo.nodenumber == p.destaddr)
    {
        CHECK(CNET_write_application(&p.msg, &p.len));
        CNET_set_LED(5, CNET_LED_OFF);
    }
    //intermediate node
    else
    {
        net_down_dll(p);
    }
}



EVENT_HANDLER(timeouts)
{
    //Oh god, i hope the queue is not empty at this point....
    if(!isEmpty(frameQ))
    {
        printf("\t\t\t\t\tTimeout: re-sending on link %i seq=%i\n",
                    frameQ->head->value.link, frameQ->head->value.seq);
        //resend stored frame
        dll_down_phys(frameQ->head->value);
    }
    else
    {
         printf("\t\t\t\t\tQueue is empty: something is VERY wrong\n");
    }
}

EVENT_HANDLER(showstate)
{
    //printf("It's cnet so: likely broken\n");
    for(int i = 1; i <= nodeinfo.nlinks; i++)
    {
        printf("\nLink %i\t"
            "incrementOnFrame = %i\t"
            "expectedACK = %i\t"
            "expectedFrame = %i\t"
            "lasttimer = %i", i, 
            incrementOnFrame[i], expectedACK[i], 
            expectedFrame[i], lasttimer[i]);
    }
}

//wrap a packet in a frame
FRAME newFrame(PACKET* p, FRAMEKIND kind, int seq, int link)
{
    FRAME f;
    f.kind = kind;
    f.checksum = 0;
    f.seq = seq;
    f.link = link;

    if(kind == DL_DATA) //DL_DATA
    {
        f.len = PACKET_SIZE((*p));
        memcpy(&f.packet, p, f.len);
    }
    else //ACK
    {
        f.len = 0;
    }

    //oh man, i love gross turnaries
    //either calculate checksum for entire frame (DL_DATA) or just for the header (ACK)
    f.checksum = CNET_ccitt((unsigned char *)&f, (f.kind == DL_DATA) ? sizeof(FRAME) : FRAME_HEADER_SIZE);

    return f;
}





















/**************************************************
**********************QUEUE************************
***************************************************/

list* newList(void){
        list* ret = (list*)malloc(sizeof(list));
        ret-> head = NULL;
        ret->tail = NULL;
        ret->count = 0;
        
        return ret;
}

listNode* newNode(void){
    listNode* ret = (listNode*)malloc(sizeof(listNode));
    ret->next = NULL;

    return ret;
}

listNode* newNode_v(FRAME inVal){
        
    listNode* ret = newNode();
    ret->value = inVal;
    
    return ret;
}

void addNode(list* theList, FRAME inVal){

        listNode* node = newNode_v(inVal);
        if(isEmpty(theList))
        {
            /*add event to an empty list*/
            theList->head = theList->tail = node;
        }
        else
        {
            /*add event to the end of a populated list*/
            theList->tail->next = node;
            theList->tail = node;
        }
        
        /*increment list count*/
        theList->count++;

}

FRAME removeNode(list* theList){
        FRAME ret;
        if(!isEmpty(theList))
        {
                ret = theList->head->value;
                freeNode(theList, theList->head);
        }
        
        return ret;
}

void freeNode(list* theList, listNode* node)
{
    if(!isEmpty(theList))
    {
        /*only one item in list*/
        if(theList->head->next == NULL)
        {
            theList->head = theList->tail = NULL;
        }
        /*if first item in a list with >1 items*/
        else if(node == theList->head)
        {
            theList->head = theList->head->next;
        }
        /*two or more items in list*/
        else
        {
            /*keep track of previous node to chnage next pointer*/
            listNode* prevNode = theList->head;
            
            /*loop until we find the node we want*/
            while(prevNode->next != node)
            {
                prevNode = prevNode->next;
            }
        
            /*assign next node to be previous node's new next node*/
            prevNode->next = node->next;
            
            /*re-assign the tail if last item in list*/
            if(node == theList->tail)
            {
                theList->tail = prevNode;
            }
        }

        /*decrement list count*/
        theList->count--;
    
        free(node);
        
    }
}

void freeList(list* theList)
{
    while(!isEmpty(theList))
    {
        freeNode(theList, theList->head);
    }
    
    free(theList);
}

int isEmpty(list* theList){
    return (theList->count == 0);
}

