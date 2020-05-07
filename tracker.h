#include<stdint.h>
#include<stdlib.h>
#include<zmq.h>
#include<nanomsg/nn.h>
#include<nanomsg/pipeline.h>
#include<string.h>

#include "chunk.h"

void chunk__dump( chunk *c );
char chunk__isheader( chunk *c );
chunk *read_chunk_non_header( FILE *fh );

typedef struct myzmq_s {
    void *context;
    void *socket;
} myzmq;

myzmq *myzmq__new( char *spec, int bind ) {
    myzmq *self = calloc( sizeof( myzmq ), 1 );
    self->context = zmq_ctx_new();
    self->socket = zmq_socket( self->context, bind ? ZMQ_PULL : ZMQ_PUSH );
    int rc;
    if( bind ) {
        rc = zmq_bind( self->socket, spec );
        if( rc ) {
            fprintf(stderr, "ZMQ could not bind to %s ; err = %i\n", spec, rc );
            exit(1);
        }
    }
    else {
        rc = zmq_connect( self->socket, spec );
        if( rc ) {
            fprintf(stderr, "ZMQ could not connect to %s ; err = %i\n", spec, rc );
            exit(1);
        }
    }
    
    return self;
}

int mynano__new( char *spec, int bind ) {
    int sock = nn_socket( AF_SP, bind ? NN_PULL : NN_PUSH );
    if( sock < 0 ) { fprintf(stderr, "nanomsg socket creation err: %i\n", sock ); exit(1); }
    int rv;
    if( bind ) rv = nn_bind( sock, spec );
    else       rv = nn_connect( sock, spec );
    if( rv < 0 ) { fprintf(stderr, "nanomsg bind/connect err: %i\n", rv ); exit(1); }
    return sock; 
}

void myzmq__del( myzmq *self ) {
    if( self ) {
        if( self->socket ) zmq_close( self->socket );
        if( self->context ) zmq_ctx_destroy( self->context );
    }
}

char *decode_err( int e ) {
    if( e == EAGAIN ) return "EAGAIN";
    if( e == ENOTSUP ) return "ENOTSUP";
    if( e == EFSM ) return "EFSM";
    if( e == ETERM ) return "ETERM";
    if( e == ENOTSOCK ) return "ENOTSOCK";
    if( e == EINTR ) return "EINTR";
    return "?";
}

char buffer[ 500000 ];

chunk *myzmq__recv_chunk( myzmq *z ) {
    int size = zmq_recv( z->socket, buffer, 500000, 0 );
    if( !size ) return NULL;
    if( size == -1 ) {
        int err = zmq_errno();
        printf("ZMQ error receiving %i ( %s )\n", err, decode_err( err ) );
        return NULL;
    }
    //printf("Received zmq chunk of size %i\n", size );
    chunk *c = calloc( sizeof( chunk ), 1 );
    c->size = size;
    c->data = malloc( size );
    c->type = buffer[4];
    c->dtype = 2;
    memcpy( c->data, buffer, size );
    chunk__dump( c );
    return c;
}

void chunk__del( chunk *c ) {
    if( !c ) return;
    if( c->dtype == 0 ) free( c->data );
    if( c->dtype == 1 ) nn_freemsg( c->data );
    if( c->dtype == 2 ) free( c->data );
    free( c );
}

chunk *mynano__recv_chunk( int n ) {
    char *buf = NULL;
    int size = nn_recv( n, &buf, NN_MSG, 0 );
    if( !size ) return NULL;
    if( size < 0 ) { fprintf(stderr, "nn_recv err %i\n", size ); return NULL; }
    
    //printf("Received nanomsg chunk of size %i\n", size );
    
    chunk *c = calloc( sizeof( chunk ), 1 );
    c->size = size;
    c->data = buf;
    c->type = buf[4];
    c->dtype = 1;
    //printf("%x %x %x %x %x\n", buffer[0], buffer[1], buffer[2], buffer[3], buffer[4] );
    chunk__dump( c );
    return c;
}

void mynano__send_chunk( int n, chunk *c ) {
    nn_send( n, c->data, c->size, 0 );
}

void myzmq__send_chunk( myzmq *z, chunk *c ) {
    zmq_send( z->socket, c->data, c->size, 0 );
}

void myzmq__send( myzmq *z, void *data, int size ) {
    zmq_send( z->socket, data, size, 0 );
}

void mynano__send( int n, void *data, int size ) {
    nn_send( n, data, size, 0 );
}

chunk *read_chunk( FILE *fh );

void tracker__del( chunk_tracker *tracker ) {
    chunk *cur = tracker->curchunk;
    while( cur ) {
        chunk *next = cur->next;
        chunk__del( cur );
        cur = next;
    }
    free( tracker );
}

void tracker__add_chunk( chunk_tracker *tracker, chunk *c ) {
    chunk *curchunk = tracker->curchunk;
    if( !curchunk ) {
        tracker->curchunk = c;
        tracker->pos = 0; // shouldn't be needed
        return;
    }
    while( curchunk->next ) {
        curchunk = curchunk->next;
    }
    curchunk->next = c;
}

void tracker__read_headers( chunk_tracker *tracker, FILE *fh ) {
    int done = 0; 
    while( !done ) {
        chunk *c = read_chunk( fh );
        if( !c ) break;
        
        if( c->type == 6 ) done = 1; // 6 = SEI
        tracker__add_chunk( tracker, c );
    }
}

void chunk__write( chunk *c, FILE *fh );

void tracker__write_file( chunk_tracker *tracker, FILE *fh ) {
    chunk *cur = tracker->curchunk;
    while( cur ) {
        chunk *next = cur->next;
        chunk__write( cur, fh );
        cur = next;
    }
}

void tracker__myzmq__send_chunks( chunk_tracker *tracker, myzmq *z ) {
    chunk *c = tracker->curchunk;
    while( c ) {
        myzmq__send_chunk( z, c );
        c = c->next;
    }
    tracker->curchunk = NULL;
    tracker->pos = 0;
}

void tracker__mynano__send_chunks( chunk_tracker *tracker, int n ) {
    chunk *c = tracker->curchunk;
    while( c ) {
        mynano__send_chunk( n, c );
        c = c->next;
    }
    tracker->curchunk = NULL;
    tracker->pos = 0;
}

void tracker__myzmq__recv_headers( chunk_tracker *tracker, myzmq *z ) {
    int done = 0;
    while( !done ) {
        chunk *c = myzmq__recv_chunk( z );
        if( !c ) break;
        if( c->type == 0 ) continue; // dummy chunk to initialize zmq
        
        if( c->type == 6 ) done = 1;
        tracker__add_chunk( tracker, c );
    }
}


//"SEI", // 6
//"SPS", // 7
//"PPS"  // 8
char tracker__mynano__recv_headers( chunk_tracker *tracker, int n ) {
    char gotSei = 0;
    char gotSps = 0;
    char gotPps = 0;
    while( !gotSei || !gotSps || !gotPps ) {
        chunk *c = mynano__recv_chunk( n );
        if( !c ) break;
        if( c->type == 0 ) continue; // dummy chunk to initialize zmq
        
        if( c->easyType == 6 ) gotSei = 1;
        else if( c->easyType == 7 ) gotSps = 1;
        else if( c->easyType == 8 ) gotPps = 1;
        else {
            printf("Got chunk type %i while trying to receive headers\n", c->easyType );
            return 0;
        }
        tracker__add_chunk( tracker, c );
    }
    return 1;
}

int tracker__read_frame( chunk_tracker *tracker, FILE *fh ) {
    chunk *c = read_chunk_non_header( fh );
    if( c ) {
        tracker__add_chunk( tracker, c );
        return 1;
    }
    printf("Could not fetch frame chunk\n");
    return 0;
}

int tracker__myzmq__recv_frame( chunk_tracker *tracker, myzmq *z ) {
    chunk *c = myzmq__recv_chunk( z );
    if( c ) {
        tracker__add_chunk( tracker, c );
        return 1;
    }
    printf("Could not fetch frame chunk\n");
    return 0;
}

int tracker__mynano__recv_frame( chunk_tracker *tracker, int n ) {
    chunk *c = mynano__recv_chunk( n );
    if( c ) {
        tracker__add_chunk( tracker, c );
        //printf("Frame type %i\n", c->easyType );
        return 1;
    }
    printf("Could not fetch frame chunk\n");
    return 0;
}

int tracker__mynano__recv_frame_non_header( chunk_tracker *tracker, int n ) {
    while( 1 ) {
        chunk *c = mynano__recv_chunk( n );
        if( !c ) {
            printf("Could not fetch frame chunk\n");
            return 0;
        }
        if( chunk__isheader( c ) ) continue;
        tracker__add_chunk( tracker, c );
        return 1;
    }
    //unreachable
    return 0;
}

struct timespec *lastI = NULL, *nextI = NULL;
char *naltypes[9] = {
    NULL, // 0
    NULL, // 1
    NULL, // 2
    NULL, // 3
    NULL, // 4
    NULL, // 5
    "SEI", // 6
    "SPS", // 7
    "PPS" // 8
};
char chunk__isheader( chunk *c ) {
    char t = c->type;
    int forbidden_zero_bit = ( t & 0x80 ) >> 7;
    int ref_idc = ( t & 0x60 ) >> 5;
    int type = ( t & 0x1F );
    return ( type == 6 || type == 7 || type == 8 );
}
void chunk__dump( chunk *c ) {
    char t = c->type;
    int forbidden_zero_bit = ( t & 0x80 ) >> 7;
    int ref_idc = ( t & 0x60 ) >> 5;
    int type = ( t & 0x1F );
    c->easyType = type;
    if( type == 1 ) {
        //if( ref_idc ) printf(".");
        //else printf("x");
    }
    else {
        if( type == 5 ) {
            if( !nextI ) nextI = malloc( sizeof( struct timespec ) ); 
            clock_gettime( CLOCK_MONOTONIC, nextI );
            if( lastI ) {
                // display time difference
                uint64_t dif = timespecDiff( nextI, lastI );
                printf(" Iframe - size: %li - Timediff:%f\n", (long) c->size, (double) dif / ( double ) 1000000 );
                free( lastI );
                lastI = nextI;
                nextI = NULL;
            }
            else {
                lastI = nextI;
                nextI = NULL;
                printf(" Iframe - size: %li\n", (long) c->size );
            }
        }
        else {
            if( type <= 9 && naltypes[type] ) {
                printf("nalu type: %s, size: %li\n", naltypes[type], (long) c->size );
            }
            else printf("nalu type: %i, size: %li\n", type, (long) c->size );
        }
    }
}

chunk *read_chunk( FILE *fh ) {
    char m[4];
    int read = fread( m, 4, 1, fh );
    if( !read ) return NULL;
    if( m[0] == 0x00 && m[1] == 0x00 && m[2] == 0x00 && m[3] == 0x01 ) {
        unsigned char data[4];
        fread( data, 4, 1, fh );
        uint32_t size = (data[3]<<0) | (data[2]<<8) | (data[1]<<16) | (data[0]<<24);
        
        char *chunkdata = malloc( size + 4 );
        chunkdata[0] = 0x00; chunkdata[1] = 0x00; chunkdata[2] = 0x00; chunkdata[3] = 0x01;
        fread( &chunkdata[4], size, 1, fh );
        chunk *c = calloc( sizeof( chunk ), 1 );
        c->data = chunkdata;
        c->type = chunkdata[4];
        c->size = size + 4;
        c->dtype = 0;
        chunk__dump( c );
        return c;
    }
    fprintf(stderr, "not magic; not a good sign\n");
    return NULL;
}

chunk *read_chunk_non_header( FILE *fh ) {
    char m[4];
    while( 1 ) {
        int read = fread( m, 4, 1, fh );
        if( !read ) return NULL;
        if( m[0] == 0x00 && m[1] == 0x00 && m[2] == 0x00 && m[3] == 0x01 ) {
            unsigned char data[4];
            fread( data, 4, 1, fh );
            uint32_t size = (data[3]<<0) | (data[2]<<8) | (data[1]<<16) | (data[0]<<24);
            
            char *chunkdata = malloc( size + 4 );
            chunkdata[0] = 0x00; chunkdata[1] = 0x00; chunkdata[2] = 0x00; chunkdata[3] = 0x01;
            fread( &chunkdata[4], size, 1, fh );
            chunk *c = calloc( sizeof( chunk ), 1 );
            c->data = chunkdata;
            c->type = chunkdata[4];
            c->size = size + 4;
            c->dtype = 0;
            chunk__dump( c );
            if( !chunk__isheader( c ) ) return c;
        }
    }
    // unreachable
    return NULL;
}

void chunk__write( chunk *c, FILE *fh ) {
    char m[4] = { 0,0,0,0x01 };
    fwrite( m, 1, 4, fh );
    char sz[4];
    long int datasize = c->size;
    sz[0] = ( datasize & 0xFF000000 ) >> 24;
    sz[1] = ( datasize & 0x00FF0000 ) >> 16;
    sz[2] = ( datasize & 0x0000FF00 ) >> 8;
    sz[3] = datasize & 0xFF;
    fwrite( sz, 1, 4, fh );
    fwrite( &c->data[4], 1, datasize, fh );  
}

chunk_tracker *tracker__new() {
    chunk_tracker *tracker = calloc( sizeof( chunk_tracker ), 1 );
    tracker->curchunk = NULL;
    tracker->pos = 0;
    return tracker;
}