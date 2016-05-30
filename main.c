#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <time.h>
#include <errno.h> // EBUSY
#include <assert.h>
#include <mpi.h>
#include <pthread.h>
#include "list.h"
#include "utilities.h"
// #include "ship.h"

#define MAX_DATA 2
#define SLEEP_INTERVAL 1000

int myNumber;
int SHIPS;
int DOCKS;
int SCVs;
Ship* lowerPriorityShips = NULL;
Ship* higherPriorityShips = NULL;
Ship* neutralShips = NULL;
int _time = 0;
int timestamp;

typedef enum {REQUEST, ACK, TERMINATE} msg_type;

pthread_mutex_t quitMutex = PTHREAD_MUTEX_INITIALIZER; // used as quit flag

bool isMyPriorityHigher(Ship *s)
{
   if(timestamp == s->timestamp)
      return myNumber < s->num;
   return timestamp < s->timestamp;
}

void agreeToPendingRequests()
{
   Ship* s = lowerPriorityShips;
   while(s != NULL)
   {
      _time++;
      int data[2] = {s->timestamp, s->dmg};
      MPI_Send(data, 2, MPI_INT, s->num, ACK, MPI_COMM_WORLD);
      s = s->next;
   }
   list_cut_paste(&lowerPriorityShips, &higherPriorityShips);
}

void checkPermission(Ship *ship)
{
   list_remove(&higherPriorityShips,ship->num);
   list_remove(&neutralShips, ship->num);
   list_remove(&lowerPriorityShips, ship->num);
   if(!isMyPriorityHigher(ship))
   {
      _time++;
      int data[2] = {ship->timestamp, ship->dmg};
      MPI_Send(data, 2, MPI_INT, ship->num, ACK, MPI_COMM_WORLD);
      list_add(&higherPriorityShips, ship);
   }
   else
   {
      list_add(&lowerPriorityShips, ship);
   }
}

/* Returns 1 (true) if the mutex is unlocked, which is the
 * thread's signal to terminate.
 */
bool needQuit()
{
  switch(pthread_mutex_trylock(&quitMutex))
  {
    case 0: /* if we got the lock, unlock and return true */
      pthread_mutex_unlock(&quitMutex);
      return true;
    case EBUSY: /* return false if the mutex was locked */
      return false;
  }
  return true;
}

int countDamage()
{
   Ship* node = higherPriorityShips;
   int totalDmg = 0;
   while(node != NULL)
   {
      totalDmg += node->dmg;
      node = node->next;
   }
   return totalDmg;
}

void cleanup()
{
   printf("%d Cleaning\n", myNumber);
   list_free(&neutralShips);
   list_free(&lowerPriorityShips);
   list_free(&higherPriorityShips);
}

void *terminationListener()
{
   char ch;
   printf("Type 'q' to terminate\n");
   while(true)
   {
      ch = getchar();
      if(ch == 'q')
      {
         pthread_mutex_unlock(&quitMutex);
         printf("Terminating program\n");
         return NULL;
      }
   }
}

int main(int argc, char* argv[])
{
   assert(argc == 3);
   int i;
   int provided;
   MPI_Init_thread(&argc, &argv, MPI_THREAD_MULTIPLE, &provided);
   // printf("%d\n", provided);

   DOCKS = atoi(argv[1]);
   SCVs = atoi(argv[2]);

   MPI_Comm_rank (MPI_COMM_WORLD, &myNumber);        /* get current process id */
   MPI_Comm_size (MPI_COMM_WORLD, &SHIPS);        /* get number of processes */
   printf("%d %d Battlecruiser number %d of %d reporting\n", _time, myNumber, myNumber, SHIPS );

   pthread_mutex_lock(&quitMutex);
   if(myNumber == 0)
   {
      pthread_t pth; // wątek do terminacji programu, zbędny w procesach innych niż root
      pthread_create(&pth, NULL, &terminationListener, NULL);
   }

   srand(time(NULL));
   bool requesting = false;
   bool repairing = false;
   int busyTime = 0;
   MPI_Request request = MPI_REQUEST_NULL;
   MPI_Status status;
   int flag = -1;
   int data[MAX_DATA];
   int dmgRcvd = 0;

   // MAIN LOOP
   while( !needQuit() )
   {
      //Losowanie czasu walki
      if(busyTime == 0 && !requesting)
         busyTime = ((rand() % 10) + 1) * 1000000; // microseconds

      //Odczekiwanie interwału
      if(busyTime > 0)
      {
         fightSpaceBears(SLEEP_INTERVAL);//in terran fightSpaceBears has the same meaning as getRepairs
         busyTime -= SLEEP_INTERVAL;
      }

      if(busyTime == 0 && !requesting)
      {
         dmgRcvd = (rand() % SCVs) + 1;
         printf("%d %d: Received %d dmg\n", _time, myNumber, dmgRcvd);
         _time++;
         timestamp = _time;
         for(i=0; i<SHIPS; i++)
            if(i != myNumber)
            {
               int data[2] = {timestamp, dmgRcvd};
               // TODO: POWINIEN BYĆ CHYBA NIEBLOKUJĄCY
               MPI_Send(data, 2, MPI_INT, i, REQUEST, MPI_COMM_WORLD);
            }

         printf("%d %d: Awaiting dock and %d repairs\n", _time, myNumber, dmgRcvd);
         requesting = true;
      }


      // RECEIVE
      if(flag != 0)
      {
         MPI_Irecv(data, MAX_DATA, MPI_INT, MPI_ANY_SOURCE, MPI_ANY_TAG, MPI_COMM_WORLD, &request);
         flag = 0;
      }
      MPI_Test(&request, &flag, &status);
      if(flag != 0)
      {
         // zawsze wysyłany timestamp i dmgRcvd
         int senderNumber = status.MPI_SOURCE;
         int senderTimestamp = data[0];
         int senderDmgRcvd = data[1];
         switch (status.MPI_TAG)
         {
            case REQUEST:
            {
               //printf("%d %d: Received REQUEST\n", _time, myNumber);
               checkPermission(make_ship(senderNumber,senderTimestamp,senderDmgRcvd));
               _time = max(_time, senderTimestamp) + 1;
               break;
            }

            case ACK:
            {
               //printf("%d %d: Received ACK\n", _time, myNumber);
               // DONE: przesyłanie dmgRcvd
               // (dodajemy do neutrali, później może trafić do higherPriorityShips)
               list_remove(&higherPriorityShips, senderNumber);
               list_remove(&neutralShips, senderNumber);
               if(senderTimestamp != timestamp)
                  break; // old, outdated ACK

               if(list_find(lowerPriorityShips, senderNumber) == NULL)
               {
                  list_add(&neutralShips, make_ship(senderNumber,senderTimestamp,senderDmgRcvd));
               }
               _time = max(_time, senderTimestamp) + 1;
               break;
            }

            case TERMINATE:
            {
               printf("%d %d: Received TERMINATE\n", _time, myNumber);

               pthread_mutex_unlock(&quitMutex); // alternatywa -- goto
               requesting = false; // coby pominać poniższe
               break;
            }
         }
      }


      // CRITICAL SECTION ENTRY
      if(requesting && !repairing)
      {
         int hpShips = list_length(higherPriorityShips);
         if( (SHIPS - 1) == list_length(neutralShips) + list_length(lowerPriorityShips) + hpShips )
         {
            if( hpShips <= (DOCKS - 1) && countDamage() <= (SCVs - dmgRcvd) )
            {
               printf("%d %d: CRITS Docking and getting repairs\n", _time, myNumber);
               //printf("%d %d: State neutral:\n", _time, myNumber);
               //print_list(neutralShips, _time, myNumber);
               //printf("%d %d: State lower:\n", _time, myNumber);
               //print_list(lowerPriorityShips, _time, myNumber);
               //printf("%d %d: State higher:\n", _time, myNumber);
               //print_list(higherPriorityShips,_time, myNumber);
               // CRITICAL SECTION BEGINS: dock and repair
               repairing = true;
               busyTime = 2000000; //Dwie sekundy
            }
         }
      }

      if (repairing && busyTime == 0){
         //CRITICAL SECTION ENDS
         list_free(&neutralShips);
         agreeToPendingRequests();
         requesting = false; // gdzieś tutaj na końcu
         repairing = false;
         printf("%d %d: CRIEND Battlecruiser operational\n", _time, myNumber);
      }

      fflush(stdout);
   }

   if(myNumber == 0)
   {
      // 'terminate' broadcast
      for(i=0; i<SHIPS; i++)
         if(i != myNumber)
            MPI_Send(0, 0, MPI_INT, i, TERMINATE, MPI_COMM_WORLD);
   }

   cleanup();
   printf("Thread %d terminated\n", myNumber);

   MPI_Finalize();
   return 0;
}
