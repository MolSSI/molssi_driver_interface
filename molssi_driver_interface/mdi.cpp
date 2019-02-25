/*! \file
 *
 * \brief Functions callable by users of the MolSSI Driver Interface
 */



/* ----------------------------------------------------------------------
   MDI - MolSSI Driver Interface
   https://molssi.org/, Molecular Sciences Software Institute
   Taylor Barnes, tbarnes1@vt.edu
-------------------------------------------------------------------------

Contents:
   MDI_Init: Initialize MDI
   MDI_Accept_Communicator: Accepts a new MDI communicator
   MDI_Send: Sends data through the socket
   MDI_Recv: Receives data through the socket
   MDI_Send_Command: Sends a string of length MDI_COMMAND_LENGTH through the
      socket
   MDI_Recv_Command: Receives a string of length MDI_COMMAND_LENGTH through the
      socket
*/

#include <signal.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>
#include <errno.h>
#include <iostream>
#include <vector>
#include "mdi.h"
#include "communicator.h"
#include "mdi_global.h"

using namespace MDI_STUBS;
using namespace std;



// has MDI_Listen or MDI_Request_Connection been called?
static int any_initialization = 0;

// has MDI_Listen or MDI_Request_Connection been called with method="MPI"?
static int mpi_initialization = 0;

// internal MPI communicator
static MPI_Comm intra_MPI_comm;

// the TCP socket, initialized by MDI_Listen when method="TCP"
static int tcp_socket = -1;

// the MPI rank, initialized by MDI_Listen when method="MPI"
static int world_rank = -1;

// the MPI rank within the code
static int intra_rank = -1;

// the MPI rank of the code
static int mpi_code_rank = 0;

void mdi_error(const char* message) {
  perror(message);
  exit(1);
}



/*----------------------------*/
/* Signal handler definitions */
/*----------------------------*/

int driver_sockfd;
void sigint_handler(int dummy) {
  close(driver_sockfd);
}

int gather_names(const char* hostname_ptr, bool do_split){
   int i, j, icomm;
   int driver_rank;
   int nunique_names = 0;

   // get the number of processes
   int world_size;
   MPI_Comm_size(MPI_COMM_WORLD, &world_size);

   // get the rank of this process
   MPI_Comm_rank(MPI_COMM_WORLD, &world_rank);

   //create the name of this process
   char buffer[MDI_NAME_LENGTH];
   int str_end;
   strcpy(buffer, hostname_ptr);

   char* names = NULL;
   names = (char*)malloc(sizeof(char) * world_size*MDI_NAME_LENGTH);

   char* unique_names = NULL;
   unique_names = (char*)malloc(sizeof(char) * world_size*MDI_NAME_LENGTH);

   MPI_Allgather(&buffer, MDI_NAME_LENGTH, MPI_CHAR, names, MDI_NAME_LENGTH,
              MPI_CHAR, MPI_COMM_WORLD);

   if (world_rank == 0) {
     for (i=0; i<world_size; i++) {
       char* ptr1 = &names[i*MDI_NAME_LENGTH];
     }
   }

   // determine which rank corresponds to rank 0 of the driver
   driver_rank = -1;
   for (i=0; i<world_size; i++) {
     if ( driver_rank == -1 ) {
       char name[MDI_NAME_LENGTH];
       memcpy( name, &names[i*MDI_NAME_LENGTH], MDI_NAME_LENGTH );
       if ( strcmp(name, "") == 0 ) {
	 driver_rank = i;
       }
     }
   }
   if ( driver_rank == -1 ) {
     perror("Unable to identify driver when attempting to connect via MPI");
   }

   //if (world_rank == 0) {

     //create communicators
     for (i=0; i<world_size; i++) {
       char name[MDI_NAME_LENGTH];
       memcpy( name, &names[i*MDI_NAME_LENGTH], MDI_NAME_LENGTH );

       int found = 0;
       for (j=0; j<i; j++) {
	 char prev_name[MDI_NAME_LENGTH];
	 memcpy( prev_name, &names[j*MDI_NAME_LENGTH], MDI_NAME_LENGTH );
	 if ( strcmp(name, prev_name) == 0 ) {
	   found = 1;
	 }
       }

       // check if this rank is the first instance of a new production code
       if ( found == 0 && strcmp(name,"") != 0 ) {
	 // add this code's name to the list of unique names
	 memcpy( &unique_names[nunique_names*MDI_NAME_LENGTH], name, MDI_NAME_LENGTH );
	 nunique_names++;
	 char my_name[MDI_NAME_LENGTH];
	 memcpy( my_name, &names[world_rank*MDI_NAME_LENGTH], MDI_NAME_LENGTH );
	 if ( strcmp(my_name, name) == 0 ) {
	   mpi_code_rank = nunique_names;
	 }

         // create a communicator to handle communication with this production code
	 MPI_Comm new_mpi_comm;
	 int color = 0;
	 int key = 0;
	 if ( world_rank == driver_rank ) {
	   color = 1;
	 }
	 else if ( world_rank == i ) {
	   color = 1;
	   key = 1;
	 }
         MPI_Comm_split(MPI_COMM_WORLD, color, key, &new_mpi_comm);

	 if ( world_rank == driver_rank || world_rank == i ) {
	   Communicator* new_communicator = new CommunicatorMPI( MDI_MPI, new_mpi_comm, key );
	 }
       }
     }

     if ( do_split ) {

       // create the intra-code communicators
       MPI_Comm_split(MPI_COMM_WORLD, mpi_code_rank, world_rank, &intra_MPI_comm);
       MPI_Comm_rank(intra_MPI_comm, &intra_rank);

       MPI_Barrier(MPI_COMM_WORLD);

     }

   return 0;
}


int MDI_Listen_TCP(int port)
{
  int ret;
  int sockfd;
  struct sockaddr_in serv_addr;
  int reuse_value = 1;

  // create the socket
  sockfd = socket(AF_INET, SOCK_STREAM, 0);
  if (sockfd < 0) {
    perror("Could not create socket");
    return -1;
  }

  // ensure that the socket is closed on sigint
  driver_sockfd = sockfd;
  signal(SIGINT, sigint_handler);

  // create the socket address
  bzero((char *) &serv_addr, sizeof(serv_addr));
  serv_addr.sin_family = AF_INET;
  serv_addr.sin_addr.s_addr = htonl(INADDR_ANY);
  serv_addr.sin_port = htons(port);

  // enable reuse of the socket
  ret = setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &reuse_value, sizeof(int));
  if (ret < 0) {
    perror("Could not reuse socket");
    return -1;
  }

  // bind the socket
  ret = bind(sockfd, (struct sockaddr *) &serv_addr, sizeof(serv_addr));
  if (ret < 0) {
    perror("Could not bind socket");
    return -1;
  }

  // start listening (the second argument is the backlog size)
  ret = listen(sockfd, 20);
  if (ret < 0) {
    perror("Could not listen");
    return -1;
  }

  //return sockfd;
  tcp_socket = sockfd;

  return 0;
}


int MDI_Request_Connection_TCP(int port, char* hostname_ptr)
{
  int ret, sockfd;

  if ( any_initialization == 0 ) {
    // create the vector for the communicators
    any_initialization = 1;
  }

  struct sockaddr_in driver_address;
  struct hostent* host_ptr;

  // get the address of the host
  printf("port: %d\n",port);
  printf("hostname: %s\n",hostname_ptr);
  host_ptr = gethostbyname((char*) hostname_ptr);
  if (host_ptr == NULL) {
    perror("Error in gethostbyname");
    return -1;
  }
  if (host_ptr->h_addrtype != AF_INET) {
    perror("Unkown address type");
    return -1;
  }

  bzero((char *) &driver_address, sizeof(driver_address));
  driver_address.sin_family = AF_INET;
  driver_address.sin_addr.s_addr = 
    ((struct in_addr *)host_ptr->h_addr_list[0])->s_addr;
  driver_address.sin_port = htons(port);

  // create the socket
  sockfd = socket(AF_INET, SOCK_STREAM, 0);
  if (sockfd < 0) {
    perror("Could not create socket");
    return -1;
  }

  // connect to the driver
  // if the connection is refused, try again
  //   this allows the production code to start before the driver
  int try_connect = 1;
  while (try_connect == 1) {
    ret = connect(sockfd, (const struct sockaddr *) &driver_address, sizeof(struct sockaddr));
    if (ret < 0 ) {
      if ( errno == ECONNREFUSED ) {

	// close the socket, so that a new one can be created
	ret = close(sockfd);
	if (ret < 0) {
	  perror("Could not close socket");
	  return -1;
	}

	// create the socket
	sockfd = socket(AF_INET, SOCK_STREAM, 0);
	if (sockfd < 0) {
	  perror("Could not create socket");
	  return -1;
	}

      }
      else { // only error out for errors other than "connection refused"
	perror("Could not connect to the driver");
	return -1;
      }

    }
    else {
      try_connect = 0;
    }
  }

  Communicator* new_communicator = new CommunicatorTCP( MDI_TCP, sockfd );

  return 0;
}




/*--------------------------*/
/* MDI function definitions */
/*--------------------------*/


/*! \brief Initialize communication through the MDI library
 *
 * If using the "-method MPI" option, this function must be called by all ranks.
 * The function returns \p 0 on a success.
 *
 * \param [in]       options
 *                   Options describing the communication method used to connect to codes
 * \param [in, out]  world_comm
 *                   On input, the MPI communicator that spans all of the codes.
 *                   On output, the MPI communicator that spans the single code corresponding to the calling rank.
 *                   Only used if the "-method MPI" option is provided.
 */
int MDI_Init(const char* options, void* world_comm)
{
  int ret;
  int sockfd;
  struct sockaddr_in serv_addr;
  int reuse_value = 1;
  char* strtol_ptr;
  int i;

  // values acquired from the input options
  char* role;
  char* method;
  char* name;
  char* hostname;
  int port;
  char* language;
  int has_role = 0;
  int has_method = 0;
  int has_name = 0;
  int has_hostname = 0;
  int has_port = 0;
  int has_language;

  // get the MPI rank
  MPI_Comm mpi_communicator;
  int mpi_rank = 0;
  if ( world_comm == NULL ) {
    mpi_communicator = 0;
    mpi_rank = 0;
  }
  else {
    mpi_communicator = *(MPI_Comm*) world_comm;
    MPI_Comm_rank(mpi_communicator, &mpi_rank);
  }

  // calculate argc
  char* argv_line = strdup(options);
  char* token = strtok(argv_line, " ");
  int argc = 0;
  while (token != NULL) {
    argc++;
    token = strtok(NULL," ");
  }

  // calculate argv
  char* argv[argc];
  argv_line = strdup(options);
  token = strtok(argv_line, " ");
  for (i=0; i<argc; i++) {
    argv[i] = token;
    token = strtok(NULL," ");
  }

  // read options
  int iarg = 0;
  while (iarg < argc) {

    //-role
    if (strcmp(argv[iarg],"-role") == 0){
      if (iarg+2 > argc) {
	mdi_error("Argument missing from -role option");
      }
      role = argv[iarg+1];
      has_role = 1;
      iarg += 2;
    }
    //-method
    else if (strcmp(argv[iarg],"-method") == 0) {
      if (iarg+2 > argc) {
	mdi_error("Argument missing from -method option");
      }
      method = argv[iarg+1];
      has_method = 1;
      iarg += 2;
    }
    //-name
    else if (strcmp(argv[iarg],"-name") == 0){
      if (iarg+2 > argc) {
	mdi_error("Argument missing from -name option");
      }
      name = argv[iarg+1];
      has_name = 1;
      iarg += 2;
    }
    //-hostname
    else if (strcmp(argv[iarg],"-hostname") == 0){
      if (iarg+2 > argc) {
	mdi_error("Argument missing from -hostname option");
      }
      hostname = argv[iarg+1];
      has_hostname = 1;
      iarg += 2;
    }
    //-port
    else if (strcmp(argv[iarg],"-port") == 0) {
      if (iarg+2 > argc) {
	mdi_error("Argument missing from -port option");
      }
      port = strtol( argv[iarg+1], &strtol_ptr, 10 );
      has_port = 1;
      iarg += 2;
    }
    //_language
    else if (strcmp(argv[iarg],"_language") == 0) {
      if (iarg+2 > argc) {
	mdi_error("Argument missing from _language option");
      }
      language = argv[iarg+1];
      has_language = 1;
      iarg += 2;
    }
    else {
      mdi_error("Unrecognized option");
    }
  }

  // ensure the -role option was provided
  if ( has_role == 0 ) {
    mdi_error("Error in MDI_Init: -role option not provided");
  }

  // ensure the -name option was provided
  if ( has_name == 0 ) {
    mdi_error("Error in MDI_Init: -name option not provided");
  }

  // determine whether the intra-code MPI communicator should be split by gather_names
  bool do_split = true;
  if ( strcmp(language, "Python") == 0 ) {
    do_split = false;
  }



  if ( any_initialization == 0 ) {
    // create the vector for the communicators
    any_initialization = 1;
  }

  if ( strcmp(role, "DRIVER") == 0 ) {
    // initialize this code as a driver

    if ( strcmp(method, "MPI") == 0 ) {
      gather_names("", do_split);
      mpi_initialization = 1;
    }
    else if ( strcmp(method, "TCP") == 0 ) {
      if ( has_port == 0 ) {
	mdi_error("Error in MDI_Init: -port option not provided");
      }
      if ( mpi_rank == 0 ) {
	MDI_Listen_TCP(port);
      }
    }
    else {
      mdi_error("Error in MDI_Init: method not recognized");
    }

  }
  else if ( strcmp(role,"ENGINE") == 0 ) {
    // initialize this code as an engine

    if ( strcmp(method, "MPI") == 0 ) {
      gather_names(name, do_split);
      mpi_initialization = 1;
    }
    else if ( strcmp(method, "TCP") == 0 ) {
      if ( has_hostname == 0 ) {
	mdi_error("Error in MDI_Init: -hostname option not provided");
      }
      if ( has_port == 0 ) {
	mdi_error("Error in MDI_Init: -port option not provided");
      }
      if ( mpi_rank == 0 ) {
	MDI_Request_Connection_TCP(port, hostname);
      }
    }
    
  }
  else {
    mdi_error("Error in MDI_Init: role not recognized");
  }

  // set the MPI communicator correctly
  if ( mpi_initialization != 0 ) {
    if ( do_split ) {
      MPI_Comm* world_comm_ptr = (MPI_Comm*) world_comm;
      *world_comm_ptr = intra_MPI_comm;
    }
  }

  free( argv_line );

  return 0;
}


/*! \brief Accept a new MDI communicator
 *
 * The function returns an MDI_Comm that describes a connection between two codes.
 * If no new communicators are available, the function returns \p MDI_NULL_COMM.
 *
 */
MDI_Comm MDI_Accept_Communicator()
{
  int connection;

  // if MDI hasn't returned some connections, do that now
  //if ( returned_comms < comms.size() ) {
  if ( returned_comms < communicators.size() ) {
    returned_comms++;
    return returned_comms;
  }

  // check for any production codes connecting via TCP
  if ( tcp_socket > 0 ) {
    //accept a connection via TCP
    connection = accept(tcp_socket, NULL, NULL);
    if (connection < 0) {
      perror("Could not accept connection");
      exit(-1);
    }

    Communicator* new_communicator = new CommunicatorTCP( MDI_TCP, connection );

    // if MDI hasn't returned some connections, do that now
    //if ( returned_comms < comms.size() ) {
    if ( returned_comms < communicators.size() ) {
      returned_comms++;
      return returned_comms;
    }
  }

  // unable to accept any connections
  return MDI_NULL_COMM;
}


/*! \brief Send data through the MDI connection
 *
 * If running with MPI, this function must be called only by rank \p 0.
 * The function returns \p 0 on a success.
 *
 * \param [in]       buf
 *                   Pointer to the data to be sent.
 * \param [in]       count
 *                   Number of values (integers, double precision floats, characters, etc.) to be sent.
 * \param [in]       datatype
 *                   MDI handle (MDI_INT, MDI_DOUBLE, MDI_CHAR, etc.) corresponding to the type of data to be sent.
 * \param [in]       comm
 *                   MDI communicator associated with the intended recipient code.
 */
int MDI_Send(const char* buf, int count, MDI_Datatype datatype, MDI_Comm comm)
{
   if ( mpi_initialization == 1 && intra_rank != 0 ) {
     perror("Called MDI_Send with incorrect rank");
   }

   Communicator* send_comm = communicators[comm-1];
   send_comm->send(buf, count, datatype);

   return 0;
}


/*! \brief Receive data through the MDI connection
 *
 * If running with MPI, this function must be called only by rank \p 0.
 * The function returns \p 0 on a success.
 *
 * \param [in]       buf
 *                   Pointer to the buffer where the received data will be stored.
 * \param [in]       count
 *                   Number of values (integers, double precision floats, characters, etc.) to be received.
 * \param [in]       datatype
 *                   MDI handle (MDI_INT, MDI_DOUBLE, MDI_CHAR, etc.) corresponding to the type of data to be received.
 * \param [in]       comm
 *                   MDI communicator associated with the connection to the sending code.
 */
int MDI_Recv(char* buf, int count, MDI_Datatype datatype, MDI_Comm comm)
{
   if ( mpi_initialization == 1 && intra_rank != 0 ) {
     perror("Called MDI_Recv with incorrect rank");
   }

   Communicator* recv_comm = communicators[comm-1];
   recv_comm->recv(buf, count, datatype);

   return 0;
}


/*! \brief Send a command of length \p MDI_COMMAND_LENGTH through the MDI connection
 *
 * If running with MPI, this function must be called only by rank \p 0.
 * The function returns \p 0 on a success.
 *
 * \param [in]       buf
 *                   Pointer to the data to be sent.
 * \param [in]       comm
 *                   MDI communicator associated with the intended recipient code.
 */
int MDI_Send_Command(const char* buf, MDI_Comm comm)
{
   if ( mpi_initialization == 1 && intra_rank != 0 ) {
     perror("Called MDI_Send_Command with incorrect rank");
   }
   int count = MDI_COMMAND_LENGTH;
   char command[MDI_COMMAND_LENGTH];

   strcpy(command, buf);
   return MDI_Send( &command[0], count, MDI_CHAR, comm );
}


/*! \brief Receive a command of length \p MDI_COMMAND_LENGTH through the MDI connection
 *
 * If running with MPI, this function must be called only by rank \p 0.
 * The function returns \p 0 on a success.
 *
 * \param [in]       buf
 *                   Pointer to the buffer where the received data will be stored.
 * \param [in]       comm
 *                   MDI communicator associated with the connection to the sending code.
 */
int MDI_Recv_Command(char* buf, MDI_Comm comm)
{
   if ( mpi_initialization == 1 && intra_rank != 0 ) {
     perror("Called MDI_Recv_Command with incorrect rank");
   }
   int count = MDI_COMMAND_LENGTH;
   int datatype = MDI_CHAR;

   return MDI_Recv( buf, count, datatype, comm );
}


/*! \brief Return a conversion factor between two units
 *
 * The function returns the conversion factor from \p in_unit to \p out_unit.
 *
 * \param [in]       in_unit
 *                   Name of the unit to convert from.
 * \param [in]       out_unit
 *                   Name of the unit to convert to.
 */
double MDI_Conversion_Factor(char* in_unit, char* out_unit)
{
  if ( strcmp(in_unit,"Angstrom") == 0 and strcmp(out_unit,"Bohr") == 0 ) {
    return MDI_ANGSTROM_TO_BOHR;
  }
  else {
    mdi_error("Unrecognized conversion requested in MDI_Conversion_Factor");
  }
}


int MDI_Get_MPI_Code_Rank()
{
  return mpi_code_rank;
}

void MDI_Set_MPI_Intra_Rank(int rank)
{
  intra_rank = rank;
}
