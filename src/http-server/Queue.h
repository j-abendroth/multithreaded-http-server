//-----------------------------------------------------------------------------
// Queue.h
// Header file for Queue ADT
// John Abendroth
//-----------------------------------------------------------------------------

#ifndef _QUEUE_H_INCLUDE_
#define _QUEUE_H_INCLUDE_
#include <stdint.h>


// Exported type --------------------------------------------------------------
typedef struct QueueObj* Queue;


// Constructors-Destructors ---------------------------------------------------

// newQueue()
// Returns reference to new empty Queue object.
Queue newQueue(void);

// freeQueue()
// Frees all heap memory associated with Queue *pQ, and sets *pQ to NULL.
void freeQueue(Queue* pQ);


// Access functions -----------------------------------------------------------

// getFront()
// Returns the value at the front of Q.
// Pre: !isEmpty(Q)
int8_t getFront(Queue Q);

// getLength()
// Returns the length of Q.
uint8_t getLength(Queue Q);

// isEmpty()
// Returns true (1) if Q is empty, otherwise returns false (0)
uint8_t isEmpty(Queue Q);


// Manipulation procedures ----------------------------------------------------

// Enqueue()
// Places new data element at the end of Q
void Enqueue(Queue Q, uint8_t data);

// Dequeue()
// Deletes element at front of Q
// Pre: !isEmpty(Q)
int8_t Dequeue(Queue Q);

#endif
