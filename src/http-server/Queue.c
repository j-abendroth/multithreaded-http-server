//-----------------------------------------------------------------------------
// Queue.c
// Implementation file for Queue ADT
// John Abendroth
//-----------------------------------------------------------------------------

#include<stdio.h>
#include<stdlib.h>
#include <unistd.h>
#include "Queue.h"

// structs --------------------------------------------------------------------

// private NodeObj type
typedef struct NodeObj{
	int8_t data;
	struct NodeObj* next;
} NodeObj;

// private Node type
typedef NodeObj* Node;

// private QueueObj type
typedef struct QueueObj{
	Node front;
	Node back;
	uint8_t length;
} QueueObj;


// Constructors-Destructors ---------------------------------------------------

// newNode()
// Returns reference to new Node object. Initializes next and data fields.
// Private.
Node newNode(int8_t data){
	Node N = (struct NodeObj*)malloc(sizeof(NodeObj));
	N->data = data;
	N->next = NULL;
	return(N);
}

// freeNode()
// Frees heap memory pointed to by *pN, sets *pN to NULL.
// Private.
void freeNode(Node* pN){
	if( pN!=NULL && *pN!=NULL ){
		free(*pN);
		*pN = NULL;
	}
}

// newQueue()
// Returns reference to new empty Queue object.
Queue newQueue(void){
	Queue Q;
	Q = (struct QueueObj*)malloc(sizeof(QueueObj));
	Q->front = Q->back = NULL;
	Q->length = 0;
	return(Q);
}


// freeQueue()
// Frees all heap memory associated with Queue *pQ, and sets *pQ to NULL.S
void freeQueue(Queue* pQ){
	if(pQ!=NULL && *pQ!=NULL) {
		while( !isEmpty(*pQ) ) {
			Dequeue(*pQ);
		}
		free(*pQ);
		*pQ = NULL;
	}
}


// Access functions -----------------------------------------------------------

// getFront()
// Returns the value at the front of Q.
// Pre: !isEmpty(Q)
int8_t getFront(Queue Q){
	if( Q==NULL ){
		dprintf(STDERR_FILENO, "Queue Error: calling getFront() on NULL Queue reference\n");
		exit(1);
	}
	if( isEmpty(Q) ){
		dprintf(STDERR_FILENO, "Queue Error: calling getFront() on an empty Queue\n");
		return(-1);
	}
	return(Q->front->data);
}

// getLength()
// Returns the length of Q.
uint8_t getLength(Queue Q){
	if( Q==NULL ){
		dprintf(STDERR_FILENO, "Queue Error: calling getLength() on NULL Queue reference\n");
		exit(1);
	}
	return(Q->length);
}

// isEmpty()
// Returns true (1) if Q is empty, otherwise returns false (0)
uint8_t isEmpty(Queue Q){
	if( Q==NULL ){
		dprintf(STDERR_FILENO, "Queue Error: calling isEmpty() on NULL Queue reference\n");
		exit(1);
	}
	return(Q->length==0);
}


// Manipulation procedures ----------------------------------------------------

// Enqueue()
// Places new data element at the end of Q
void Enqueue(Queue Q, uint8_t data)
{
	Node N = newNode(data);

	if( Q==NULL ){
		dprintf(STDERR_FILENO, "Queue Error: calling Enqueue() on NULL Queue reference\n");
		exit(1);
	}
	if( isEmpty(Q) ) {
		Q->front = Q->back = N;
	}else{
		Q->back->next = N;
		Q->back = N;
	}
	Q->length++;
}

// Dequeue()
// Deletes element at front of Q
// Pre: !isEmpty(Q)
int8_t Dequeue(Queue Q){
	Node N = NULL;

	if( Q==NULL ){
		dprintf(STDERR_FILENO, "Queue Error: calling Dequeue() on NULL Queue reference\n");
		exit(1);
	}
	if( isEmpty(Q) ){
		//calling dequeue while it's empty is now intended
		//dprintf(STDERR_FILENO, "Queue Error: calling Dequeue on an empty Queue\n");
		return(-1);
	}
	N = Q->front;
	int8_t temp_data = N->data;
	if( getLength(Q)>1 ) {
		Q->front = Q->front->next;
	}else{
		Q->front = Q->back = NULL;
	}
	Q->length--;
	freeNode(&N);
	return(temp_data);
}
