/*
$Id$
    OWFS -- One-Wire filesystem
    OWHTTPD -- One-Wire Web Server
    Written 2003 Paul H Alfille
    email: palfille@earthlink.net
    Released under the GPL
    See the header file: ow.h for full attribution
    1wire/iButton system from Dallas Semiconductor
*/

#include "owfs_config.h"
#include "ow.h"
#include "ow_counters.h"
#include "ow_connection.h"

#include <sys/stat.h>
#include <string.h>

/* ------- Prototypes ----------- */
static int FS_write_seek(const char *buf, const size_t size, const off_t offset, struct connection_in * in, const struct parsedname * pn) ;
static int FS_real_write(const char * const buf, const size_t size, const off_t offset , const struct parsedname * pn) ;
static int FS_gamish(const char * const buf, const size_t size, const off_t offset , const struct parsedname * const pn) ;
static int FS_w_all(const char * const buf, const size_t size, const off_t offset , const struct parsedname * const pn) ;
static int FS_w_split(const char * const buf, const size_t size, const off_t offset , const struct parsedname * const pn) ;
static int FS_parse_write(const char * const buf, const size_t size, const off_t offset , const struct parsedname * const pn) ;

static int FS_input_yesno( int * const result, const char * const buf, const size_t size ) ;
static int FS_input_integer( int * const result, const char * const buf, const size_t size ) ;
static int FS_input_unsigned( unsigned int * const result, const char * const buf, const size_t size ) ;
static int FS_input_float( FLOAT * const result, const char * const buf, const size_t size ) ;
static int FS_input_date( DATE * const result, const char * const buf, const size_t size ) ;

static int FS_input_yesno_array( int * const results, const char * const buf, const size_t size, const struct parsedname * const pn ) ;
static int FS_input_unsigned_array( unsigned int * const results, const char * const buf, const size_t size, const struct parsedname * const pn ) ;
static int FS_input_integer_array( int * const results, const char * const buf, const size_t size, const struct parsedname * const pn ) ;
static int FS_input_float_array( FLOAT * const results, const char * const buf, const size_t size, const struct parsedname * const pn ) ;
static int FS_input_date_array( DATE * const results, const char * const buf, const size_t size, const struct parsedname * const pn ) ;

/* ---------------------------------------------- */
/* Filesystem callback functions                  */
/* ---------------------------------------------- */

/* Note on return values: */
/* Top level FS_write will return size if ok, else a negative number */
/* Each lower level function called will return 0 if ok, else non-zero */

/* Note on size and offset: */
/* Buffer length (and requested data) is size bytes */
/* writing should start after offset bytes in original data */
/* only binary, and ascii data support offset in single data points */
/* only binary supports offset in array data */
/* size and offset are vetted against specification data size and calls */
/*   outside of this module will not have buffer overflows */
/* I.e. the rest of owlib can trust size and buffer to be legal */

/* Format of input,
        Depends on "filetype"
        type     function    format                         Handled as
        integer  strol      decimal integer                 integer array
        unsigned strou      decimal integer                 unsigned array
        bitfield strou      decimal integer                 unsigned array
        yesno    strcmp     "0" "1" "yes" "no" "on" "off"   unsigned array
        float    strod      decimal floating point          double array
        date     strptime   "Jan 01, 1901", etc             date array
        ascii    strcpy     string without "," or null      comma-separated-strings
        binary   memcpy     fixed length binary string      binary "string"
*/


/* return size if ok, else negative */
int FS_write(const char *path, const char *buf, const size_t size, const off_t offset) {
    struct parsedname pn ;
    int r ;

    LEVEL_CALL("WRITE path=%s size=%d offset=%d\n",SAFESTRING(path),(int)size,(int)offset)

    /* if readonly exit */
    if ( readonly ) return -EROFS ;

    if ( FS_ParsedName( path , &pn ) ) {
        r = -ENOENT;
    } else if ( pn.dev==NULL || pn.ft == NULL ) {
        r = -EISDIR ;
    } else if (pn.type == pn_structure ) { /* structure is read-only */
        r = -ENOTSUP ;
    } else if ( pn.in == NULL ) {
        r = -ENODEV ;
    } else {
        r = FS_write_postparse( buf, size, offset, &pn ) ;
    }

    FS_ParsedName_destroy(&pn) ;
    return r ; /* here's where the size is used! */
}

/* return size if ok, else negative */
int FS_write_postparse(const char *buf, const size_t size, const off_t offset, const struct parsedname * pn) {
    int r ;

    STATLOCK;
      AVERAGE_IN(&write_avg)
      AVERAGE_IN(&all_avg)
      ++ write_calls ; /* statistics */
    STATUNLOCK;

    /* if readonly exit */
    if ( readonly ) return -EROFS ;

    switch (pn->type) {
    case pn_structure:
        r = -ENOTSUP ;
        break;
    case pn_system:
    case pn_settings:
    case pn_statistics:
        //printf("FS_write_postparse: pid=%ld system/settings/statistics\n", pthread_self());
        if ( pn->state & pn_bus ) {
            /* this will either call ServerWrite or FS_real_write */
            r = FS_write_seek(buf, size, offset, pn->in, pn) ;
        } else {
            r = FS_real_write(buf, size, offset, pn) ;
        }
        break;
    default: // pn_real
//printf("FS_write_postparse: pid=%ld call fs_write_seek size=%ld\n", pthread_self(), size);

        /* handle DeviceSimultaneous */
        if(pn->dev == DeviceSimultaneous) {
            /* writing to /simultaneous/temperature will write to ALL
            * available bus.?/simultaneous/temperature
            * not just /simultaneous/temperature
            */
            r = FS_write_seek(buf, size, offset, pn->in, pn) ;
        } else {
            /* real data -- go through device chain */
            if( !(pn->state & pn_bus)) {
                struct parsedname pn2;
                int bus_nr = -1;
                if(Cache_Get_Device(&bus_nr, pn)) {
                    //printf("Cache_Get_Device didn't find bus_nr\n");
                    /* Cache_Add_Device() is called in FS_write_seek() */
                    bus_nr = CheckPresence(pn);
                }
                if(bus_nr >= 0) {
                    memcpy(&pn2, pn, sizeof(struct parsedname));
                    /* fake that we write from only one indevice now! */
                    pn2.in = find_connection_in(bus_nr);
                    pn2.state |= pn_bus ;
                    pn2.bus_nr = bus_nr ;
                    //printf("write only to bus_nr=%d\n", bus_nr);
                    r = FS_write_seek(buf, size, offset, pn2.in, &pn2) ;
                } else {
                    //printf("CheckPresence failed, no use to write\n");
                    r = -ENOENT ;
                }
            } else {
                r = FS_write_seek(buf, size, offset, pn->in, pn) ;
            }
        }
    }

    STATLOCK;
    if ( r == 0 ) {
      ++write_success ; /* statistics */
      write_bytes += size ; /* statistics */
      r = size ; /* here's where the size is used! */
    }
    AVERAGE_OUT(&write_avg)
    AVERAGE_OUT(&all_avg)
    STATUNLOCK;

    return r ;
}

/* return 0 if ok, else negative */
/* Strategy is to go through all "connection_in" adapters
 * return the first successful one
 */
#ifdef OW_MT
struct write_seek_struct {
    const char * buf ;
    size_t size ;
    off_t offset ;
    struct connection_in * in ;
    const struct parsedname * pn ;
    int ret ;
} ;
 
static void * FS_write_seek_callback( void * vp ) {
    struct write_seek_struct * wss = (struct write_seek_struct *) vp ;
    wss->ret = FS_write_seek(wss->buf,wss->size,wss->offset,wss->in,wss->pn) ;
    pthread_exit(NULL);
    return NULL ;
}

static int FS_write_seek(const char *buf, const size_t size, const off_t offset, struct connection_in * in, const struct parsedname * pn) {
    int ret ;
    struct parsedname pn2 ;
    struct write_seek_struct wss = { buf, size, offset, in->next, pn, 0 } ;
    pthread_t thread ;
    int threadbad = 1;

    if(!(pn->state & pn_bus)) {
        threadbad = in->next==NULL || pthread_create( &thread, NULL, FS_write_seek_callback, &wss ) ;
    }

    memcpy( &pn2, pn, sizeof(struct parsedname) ) ; // shallow copy
    pn2.in = in ;
    
    if ( TestConnection(&pn2) ) {
        ret = -ECONNABORTED ;
    } else if ( (get_busmode(in) == bus_remote) ) {
        ret = ServerWrite( buf, size, offset, &pn2 ) ;
    } else {
        /* if readonly exit */
        if ( readonly ) return -EROFS ;

        if ( (ret=LockGet(&pn2))==0 ) {
            ret = FS_real_write( buf, size, offset, &pn2 ) ;
            LockRelease(&pn2) ;
        }
    }
    /* If sucessfully writing a device, we know it exists on a specific bus.
    * Update the cache content */
    if( (pn->type==pn_real) && (ret == 0) ) {
        Cache_Add_Device(in->index, pn);
    }

    if ( threadbad == 0 ) { /* was a thread created? */
        void * v ;
        if ( pthread_join( thread, &v ) ) return ret ; /* wait for it (or return only this result) */
        if ( wss.ret == 0 ) return 0 ; /* is it an error return? Then return this one */
    }
    return ret ;
}

#else /* OW_MT */

static int FS_write_seek(const char *buf, const size_t size, const off_t offset, struct connection_in * in, const struct parsedname * pn) {
    int ret ;
    struct parsedname pn2 ;

    memcpy( &pn2, pn, sizeof(struct parsedname) ) ; //shallow copy
    pn2.in = in ;
    if ( TestConnection(&pn2) ) {
    ret = -ECONNABORTED ;
    } else if ( (get_busmode(in) == bus_remote) ) {
        ret = ServerWrite( buf, size, offset, &pn2 ) ;
    } else {
        /* if readonly exit */
        if ( readonly ) return -EROFS ;

        if ( (ret=LockGet(&pn2))==0 ) {
            ret = FS_real_write( buf, size, offset, &pn2 ) ;
            LockRelease(&pn2) ;
        }
    }
    /* If sucessfully writing a device, we know it exists on a specific bus.
    * Update the cache content */
    if( (pn2.type==pn_real) && (ret == 0) ) {
        Cache_Add_Device(in->index, &pn2);
    }

    if ( ret && in->next ) return FS_write_seek(buf,size,offset,in->next,pn) ;
    return ret ;
}
#endif /* OW_MT */

/* return 0 if ok */
static int FS_real_write(const char * const buf, const size_t size, const off_t offset, const struct parsedname * pn) {
    int i, r = 0;
    //printf("FS_real_write\n");

    /* Writable? */
    if ( (pn->ft->write.v) == NULL ) return -ENOTSUP ;

    /* Do we exist? Only test static cases */
    /* Already parsed -- presence  check done there! on non-static */
    //if ( ShouldCheckPresence(pn) && pn->ft->change==ft_static && Check1Presence(pn) ) return -ENOENT ;

    /* Array properties? Write all together if aggregate */
    if ( pn->ft->ag ) {
        switch( pn->ft->ag->combined) {
        case ag_aggregate:
            /* agregate property -- need to read all and replace a single value, then write all */
            if ( pn->extension > -1 ) return FS_w_split(buf,size,offset,pn) ;
            /* fallthrough for extension==-1 or -2 */
        case ag_mixed:
            if ( pn->extension == -1 ) return FS_gamish(buf,size,offset,pn) ;
            /* Does the right thing, aggregate write for ALL and individual for splits */
            break ; /* continue for bitfield */
        case ag_separate:
            /* write all of them, but one at a time */
            if ( pn->extension == -1 ) return FS_w_all(buf,size,offset,pn) ;
            break ; /* fall through for individual writes */
        }
    }

    /* write individual entries */
    for(i=0; i<3; i++) {
        STAT_ADD1(write_tries[i]) ; /* statitics */
        r = FS_parse_write( buf, size, offset, pn ) ;
        if ( r==0 ) break;
    }
    LEVEL_DATA("Write error on %s (size=%d)\n",pn->path,(int)size) ;
    return r ;
}

/* return 0 if ok */
/* write a single element */
static int FS_parse_write(const char * const buf, const size_t size, const off_t offset , const struct parsedname * const pn) {
    size_t fl = FileLength(pn) ;
    int ret ;
    char * cbuf = NULL ;
//printf("FS_parse_write\n");

#ifdef OW_CACHE
    /* buffer for storing parsed data to cache */
    if ( IsLocalCacheEnabled(pn) ) cbuf = (char *) malloc( fl ) ;
#endif /* OW_CACHE */

    switch( pn->ft->format ) {
    case ft_integer:
        if ( offset ) {
            ret = -EADDRNOTAVAIL ;
        } else {
            int I ;
            ret = FS_input_integer( &I, buf, size ) ;
            if ( cbuf && ret==0 ) FS_output_integer(I,cbuf,fl,pn) ; /* post-parse cachable string creation */
            ret = ret || (pn->ft->write.i)(&I,pn) ;
        }
        break ;
    case ft_bitfield:
    case ft_unsigned:
        if ( offset ) {
            ret = -EADDRNOTAVAIL ;
        } else {
            unsigned int U ;
            ret = FS_input_unsigned( &U, buf, size ) ;
            if ( cbuf && ret==0 ) FS_output_unsigned(U,cbuf,fl,pn) ; /* post-parse cachable string creation */
            ret = ret || (pn->ft->write.u)(&U,pn) ;
        }
        break ;
    case ft_tempgap :
    case ft_float:
    case ft_temperature:
        if ( offset ) {
            ret = -EADDRNOTAVAIL ;
        } else {
            FLOAT F ;
            ret = FS_input_float( &F, buf, size ) ;
            if ( pn->ft->format == ft_temperature ) {
                F = fromTemperature( F , pn ) ;
            } else if ( pn->ft->format == ft_tempgap ) {
                F = fromTempGap( F , pn ) ;
            }
            if ( cbuf && ret==0 ) FS_output_float(F,cbuf,fl,pn) ; /* post-parse cachable string creation */
            ret = ret || (pn->ft->write.f)(&F,pn) ;
        }
        break ;
    case ft_date:
        if ( offset ) {
            ret = -EADDRNOTAVAIL ;
        } else {
            DATE D ;
            ret = FS_input_date( &D, buf, size ) ;
            if ( cbuf && ret==0 ) FS_output_date(D,cbuf,fl,pn) ; /* post-parse cachable string creation */
            ret = ret || (pn->ft->write.d)(&D,pn) ;
        }
        break ;
    case ft_yesno:
        if ( offset ) {
            ret = -EADDRNOTAVAIL ;
        } else {
            int Y ;
            ret = FS_input_yesno( &Y, buf, size ) ;
            if ( cbuf && ret==0 ) FS_output_integer(Y,cbuf,fl,pn) ; /* post-parse cachable string creation */
            ret = ret || (pn->ft->write.y)(&Y,pn) ;
        }
        break ;
    case ft_ascii:
        {
            size_t s = fl ;
            s -= offset ;
            if ( s > size ) s = size ;
            ret = (pn->ft->write.a)(buf,s,offset,pn) ;
            if ( cbuf && ret==0 ) strncpy(cbuf,buf,s) ; /* post-parse cachable string creation */
        }
        break ;
    case ft_binary:
        {
            size_t s = fl ;
            s -= offset ;
            if ( s > size ) s = size ;
            ret = (pn->ft->write.b)(buf,s,offset,pn) ;
            if ( cbuf && ret==0 ) memcpy(cbuf,buf,s) ; /* post-parse cachable string creation */
        }
        break ;
    case ft_directory:
    case ft_subdir:
        ret = -ENOSYS ;
        break ;
    default:    /* Unknown data type */
        ret = -EINVAL ;
        break ;
    }

    /* Add to cache? */
    if ( cbuf && ret==0 ) {
      //printf("CACHEADD: [%i] %s\n",strlen(cbuf),cbuf);
        Cache_Add( cbuf, strlen(cbuf), pn ) ;
    } else if ( IsLocalCacheEnabled(pn) ) {
      //printf("CACHEDEL: %s\n",pn->path);
            Cache_Del( pn ) ;
    }
    /* free cache string buffer */
    if ( cbuf ) free(cbuf) ;

    //printf("FS_parse_write: return %d\n", ret);
    return ret ;
}

/* return 0 if ok */
/* write aggregate all */
static int FS_gamish(const char * const buf, const size_t size, const off_t offset , const struct parsedname * const pn) {
    size_t elements = pn->ft->ag->elements ;
    size_t ffl = FullFileLength(pn);
    int ret ;
    char * cbuf = NULL ;

#ifdef OW_CACHE
    /* buffer for storing parsed data to cache */
    if ( IsLocalCacheEnabled(pn) ) cbuf = (char *) malloc( ffl ) ;
#endif /* OW_CACHE */

    if ( offset ) return -EADDRNOTAVAIL ;

    switch( pn->ft->format ) {
    case ft_integer:
        {
            int * i = (int *) calloc( elements , sizeof(int) ) ;
            if ( i==NULL ) {
                ret = -ENOMEM ;
            } else {
                if ( (ret = FS_input_integer_array( i, buf, size, pn ))==0 ) {
                    if ( cbuf ) FS_output_integer_array(i,cbuf,ffl,pn) ; /* post-parse cachable string creation */
                    ret = (pn->ft->write.i)(i,pn) ;
                }
            free(i) ;
            }
        }
        break ;
    case ft_unsigned:
        {
            unsigned int * u = (unsigned int *) calloc( elements , sizeof(unsigned int) ) ;
            if ( u==NULL ) {
                ret = -ENOMEM ;
            } else {
                if ( (ret = FS_input_unsigned_array( u, buf, size, pn )) == 0 ) {
                    if ( cbuf ) FS_output_unsigned_array(u,cbuf,ffl,pn) ; /* post-parse cachable string creation */
                    ret = (pn->ft->write.u)(u,pn) ;
                }
            free(u) ;
            }
        }
        break ;
    case ft_tempgap:
    case ft_float:
    case ft_temperature:
        {
            FLOAT * f = (FLOAT *) calloc( elements , sizeof(FLOAT) ) ;
            if ( f==NULL ) {
                ret = -ENOMEM ;
            } else {
                if ( (ret = FS_input_float_array( f, buf, size, pn ))==0 ) {
                    if ( pn->ft->format == ft_temperature ) {
                        size_t i ;
                        for ( i=0 ; i<elements ; ++i ) f[i] = fromTemperature(f[i],pn) ;
                    } else if ( pn->ft->format == ft_tempgap ) {
                        size_t i ;
                        for ( i=0 ; i<elements ; ++i ) f[i] = fromTempGap(f[i],pn) ;
                    }
                    if ( cbuf ) FS_output_float_array(f,cbuf,ffl,pn) ; /* post-parse cachable string creation */
                    ret = (pn->ft->write.f)(f,pn) ;
                }
            free(f) ;
            }
        }
        break ;
    case ft_date:
        {
            DATE * d = (DATE *) calloc( elements , sizeof(DATE) ) ;
            if ( d==NULL ) {
                ret = -ENOMEM ;
            } else {
                if ( (ret = FS_input_date_array( d, buf, size, pn )) ==0 ) {
                    if ( cbuf ) FS_output_date_array(d,cbuf,ffl,pn) ; /* post-parse cachable string creation */
                    ret = (pn->ft->write.d)(d,pn) ;
                }
            free(d) ;
            }
        }
        break ;
    case ft_yesno:
        {
            int * y = (int *) calloc( elements , sizeof(int) ) ;
            if ( y==NULL ) {
                ret = -ENOMEM ;
            } else {
                if ( (ret = FS_input_yesno_array( y, buf, size, pn )) == 0 ) {
                    if ( cbuf ) FS_output_integer_array(y,cbuf,ffl,pn) ; /* post-parse cachable string creation */
                    ret = (pn->ft->write.y)(y,pn) ;
                }
            free(y) ;
            }
        }
        break ;
    case ft_bitfield:
        {
            int * y = (int *) calloc( elements , sizeof(int) ) ;
            if ( y==NULL ) {
                ret = -ENOMEM ;
            } else {
                int i ;
                unsigned int U = 0 ;
                if ( (ret = FS_input_yesno_array( y, buf, size, pn )) == 0 ) {
                    for (i=pn->ft->ag->elements-1;i>=0;--i) U = (U<<1) | (y[i]&0x01) ;
                    if ( cbuf ) FS_output_integer_array(y,cbuf,ffl,pn) ; /* post-parse cachable string creation */
                    ret = (pn->ft->write.u)(&U,pn) ;
                }
            free(y) ;
            }
        }
        break ;
    case ft_ascii:
        {
            size_t s = ffl ;
            if ( offset > s ) {
                ret = -ERANGE ;
            } else {
                s -= offset ;
                if ( s > size ) s = size ;
                ret = (pn->ft->write.a)(buf,s,offset,pn) ;
                if ( cbuf && ret==0 ) strncpy(cbuf,buf,s) ; /* post-parse cachable string creation */
            }
        }
        break ;
    case ft_binary:
        {
            size_t s = ffl ;
            if ( offset > s ) {
                ret = -ERANGE ;
            } else {
                s -= offset ;
                if ( s > size ) s = size ;
                ret = (pn->ft->write.b)(buf,s,offset,pn) ;
                if ( cbuf && ret==0 ) memcpy(cbuf,buf,s) ; /* post-parse cachable string creation */
            }
        }
        break ;
    case ft_directory:
    case ft_subdir:
        ret = -ENOSYS ;
        break ;
    default:    /* Unknown data type */
        ret = -EINVAL ;
        break ;
    }

    /* Add to cache? */
    if ( cbuf && ret==0 ) {
        Cache_Add( cbuf, strlen(cbuf), pn ) ;
//printf("CACHEADD: [%i] %s\n",strlen(cbuf),cbuf);
    } else if ( IsLocalCacheEnabled(pn) ) {
            Cache_Del( pn ) ;
    }
    /* free cache string buffer */
    if ( cbuf ) free(cbuf) ;

    return ret ;
}

/* Non-combined input  field, so treat  as several separate transactions */
/* return 0 if ok */
static int FS_w_all(const char * const buf, const size_t size, const off_t offset , const struct parsedname * const pn) {
    size_t left = size ;
    const char * p = buf ;
    int r ;
    struct parsedname pname ;
//printf("WRITE_ALL\n");

    STAT_ADD1(write_array) ; /* statistics */
    memcpy( &pname , pn , sizeof(struct parsedname) ) ; /* shallow copy */
//printf("WRITEALL(%p) %s\n",p,path) ;
    if ( offset ) return -ERANGE ;

    if ( pname.ft->format==ft_binary ) { /* handle binary differently, no commas */
        int suglen = pname.ft->suglen ;
        for ( pname.extension=0 ; pname.extension < pname.ft->ag->elements ; ++pname.extension ) {
            if ( (int) left < suglen ) return -ERANGE ;
            if ( (r=FS_parse_write(p,(size_t) suglen,(const off_t)0,&pname)) ) return r ;
            p += suglen ;
            left -= suglen ;
        }
    } else { /* comma separation */
        for ( pname.extension=0 ; pname.extension < pname.ft->ag->elements ; ++pname.extension ) {
            char * c = memchr( p , ',' , left ) ;
            if ( c==NULL ) {
                if ( (r=FS_parse_write(p,left,(const off_t)0,&pname)) ) return r ;
                p = buf + size ;
                left = 0 ;
            } else {
                if ( (r=FS_parse_write(p,(size_t)(c-p),(const off_t)0,&pname)) ) return r ;
                p = c + 1 ;
                left = size - (buf-p) ;
            }
        }
    }
    return 0 ;
}

/* Combined field, so read all, change the relevant field, and write back */
/* return 0 if ok */
static int FS_w_split(const char * const buf, const size_t size, const off_t offset , const struct parsedname * const pn) {
    size_t elements = pn->ft->ag->elements ;
    int ret = 0;

    const size_t ffl = FullFileLength(pn) ;
    char * cbuf = NULL ;

    /* readable at all? cannot write a part if whole can't be read */
    if ( pn->ft->read.v == NULL ) return -EFAULT ;
    
#ifdef OW_CACHE
    cbuf = (char *) malloc( FullFileLength(pn)) ;
#endif /* OW_CACHE */

//printf("WRITE_SPLIT\n");

    switch( pn->ft->format ) {
    case ft_yesno:
        if ( offset ) {
            ret = -EADDRNOTAVAIL ;
        } else {
            int * y = (int *) calloc( elements , sizeof(int) ) ;
            if ( y==NULL ) {
                ret = -ENOMEM ;
            } else {
                ret = ((pn->ft->read.y)(y,pn)<0) || FS_input_yesno(&y[pn->extension],buf,size) || (pn->ft->write.y)(y,pn)  ;
                if ( cbuf && ret==0 ) FS_output_integer_array(y,cbuf,ffl,pn) ;
            free( y ) ;
            }
        }
        break ;
    case ft_integer:
        if ( offset ) {
            ret = -EADDRNOTAVAIL ;
        } else {
            int * i = (int *) calloc( elements , sizeof(int) ) ;
            if ( i==NULL ) {
                ret = -ENOMEM ;
            } else {
                ret = ((pn->ft->read.i)(i,pn)<0) || FS_input_integer(&i[pn->extension],buf,size) || (pn->ft->write.i)(i,pn) ;
                if ( cbuf && ret==0 ) FS_output_integer_array(i,cbuf,ffl,pn) ;
            free( i ) ;
            }
        }
        break ;
    case ft_unsigned:
        if ( offset ) {
            ret = -EADDRNOTAVAIL ;
        } else {
            unsigned int * u = (unsigned int *) calloc( elements , sizeof(unsigned int) ) ;
            if ( u==NULL ) {
                ret = -ENOMEM ;
            } else {
                ret = ((pn->ft->read.u)(u,pn)<0) || FS_input_unsigned(&u[pn->extension],buf,size) || (pn->ft->write.u)(u,pn) ;
                if ( cbuf && ret==0 ) FS_output_unsigned_array(u,cbuf,ffl,pn) ;
            free( u ) ;
            }
        }
        break ;
    case ft_bitfield:
        if ( offset ) {
            ret = -EADDRNOTAVAIL ;
        } else {
            unsigned int U ;
            int y ;
            ret = ((pn->ft->read.u)(&U,pn)<0) || FS_input_unsigned(&y,buf,size) ;
            if ( ret==0) {
                UT_setbit((void*)(&U),pn->extension,y) ;
                ret = (pn->ft->write.u)(&U,pn) ;
            }
            if ( cbuf && ret==0 ) FS_output_unsigned(U,cbuf,ffl,pn) ;
        }
        break ;
    case ft_tempgap:
    case ft_float:
            if ( offset ) {
                ret = -EADDRNOTAVAIL ;
        } else {
            FLOAT * f = (FLOAT *) calloc( elements , sizeof(FLOAT) ) ;
            if ( f==NULL ) {
                ret = -ENOMEM ;
            } else {
                ret = ((pn->ft->read.f)(f,pn)<0) || FS_input_float(&f[pn->extension],buf,size) || (pn->ft->write.f)(f,pn) ;
                if ( cbuf && ret==0 ) FS_output_float_array(f,cbuf,ffl,pn) ;
            free(f) ;
            }
        }
        break ;
    case ft_temperature:
        if ( offset ) {
            ret = -EADDRNOTAVAIL ;
        } else {
            FLOAT * f = (FLOAT *) calloc( elements , sizeof(FLOAT) ) ;
            if ( f==NULL ) {
                ret = -ENOMEM ;
            } else {
                if ( (ret=((pn->ft->read.f)(f,pn)<0))==0 ) {
                    if ( (ret=FS_input_float(&f[pn->extension],buf,size))==0 ) {
                        f[pn->extension] = fromTemperature(f[pn->extension],pn) ;
                        if ( (ret=(pn->ft->write.f)(f,pn))==0 ) {
                            if ( cbuf ) FS_output_float_array(f,cbuf,ffl,pn) ;
                        }
                    }
                }
                free(f) ;
            }
        }
        break ;
    case ft_date: {
        if ( offset ) {
            ret = -EADDRNOTAVAIL ;
        } else {
            DATE * d = (DATE *) calloc( elements , sizeof(DATE) ) ;
            if ( d==NULL ) {
                ret = -ENOMEM ;
            } else {
                ret = ((pn->ft->read.d)(d,pn)<0) || FS_input_date(&d[pn->extension],buf,size) || (pn->ft->write.d)(d,pn) ;
                if ( cbuf && ret==0 ) FS_output_date_array(d,cbuf,ffl,pn) ;
            free(d) ;
            }
        }
        break ;
    case ft_binary: {
        unsigned char * all ;
        int suglen = pn->ft->suglen ;
        size_t s = suglen ;
        if ( offset > suglen ) {
            ret = -ERANGE ;
        } else {
            s -= offset ;
            if ( s>size ) s = size ;
            if ( (all = (unsigned char *) malloc( ffl ) ) ) { ;
                if ( (ret = (pn->ft->read.b)(all,ffl,(const off_t)0,pn))==0 ) {
                    memcpy(&all[suglen*pn->extension+offset],buf,s) ;
                    ret = (pn->ft->write.b)(all,ffl,(const off_t)0,pn) ;
                    if ( cbuf && ret == 0 ) memcpy( cbuf, all, ffl ) ;
                }
                free( all ) ;
            } else {
                ret = -ENOMEM ;
            }
        }
        }
        break ;
    case ft_ascii:
        if ( offset ) {
            return -EADDRNOTAVAIL ;
        } else {
            char * all ;
            int suglen = pn->ft->suglen ;
            size_t s = suglen ;
            if ( s>size ) s = size ;
            if ( (all=(char *) malloc(ffl)) ) {
                if ((ret = (pn->ft->read.a)(all,ffl,(const off_t)0,pn))==0 ) {
                    memcpy(&all[(suglen+1)*pn->extension],buf,s) ;
                    ret = (pn->ft->write.a)(all,ffl,(const off_t)0,pn) ;
                    if ( cbuf && ret == 0 ) memcpy( cbuf, all, ffl ) ;
                }
                free( all ) ;
            } else
                ret = -ENOMEM ;
            }
        }
        break ;
    case ft_directory:
    case ft_subdir:
        ret = -ENOSYS ;
    }

    /* Add to cache? */
    if ( cbuf && ret==0 ) {
        Cache_Add( cbuf, strlen(cbuf), pn ) ;
//printf("CACHEADD: [%i] %s\n",strlen(cbuf),cbuf);
    } else if ( IsLocalCacheEnabled(pn) ) {
            Cache_Del( pn ) ;
    }
    /* free cache string buffer */
    if ( cbuf ) free(cbuf) ;

    return ret ? -EINVAL : 0 ;
}

/* return 0 if ok */
static int FS_input_yesno( int * const result, const char * const buf, const size_t size ) {
//printf("yesno size=%d, buf=%s\n",size,buf);
    if ( size ) {
        if ( buf[0]=='1' || strncasecmp("on",buf,2)==0 || strncasecmp("yes",buf,2)==0 ) {
            *result = 1 ;
//printf("YESno\n");
            return 0 ;
        }
        if ( buf[0]=='0' || strncasecmp("off",buf,2)==0 || strncasecmp("no",buf,2)==0 ) {
            *result = 0 ;
//printf("yesNO\n") ;
            return 0 ;
        }
    }
    return 1 ;
}

/* return 0 if ok */
static int FS_input_integer( int * const result, const char * const buf, const size_t size ) {
    char cp[size+1] ;
    char * end ;

    memcpy( cp, buf, size ) ;
    cp[size] = '\0' ;
    errno = 0 ;
    * result = strtol( cp,&end,10) ;
    return end==cp || errno ;
}

/* return 0 if ok */
static int FS_input_unsigned( unsigned int * const result, const char * const buf, const size_t size ) {
    char cp[size+1] ;
    char * end ;

    memcpy( cp, buf, size ) ;
    cp[size] = '\0' ;
    errno = 0 ;
    * result = strtoul( cp,&end,10) ;
//printf("UI str=%s, val=%u\n",cp,*result) ;
    return end==cp || errno ;
}

/* return 0 if ok */
static int FS_input_float( FLOAT * const result, const char * const buf, const size_t size ) {
    char cp[size+1] ;
    char * end ;

    memcpy( cp, buf, size ) ;
    cp[size] = '\0' ;
    errno = 0 ;
    * result = strtod( cp,&end) ;
    return end==cp || errno ;
}

/* return 0 if ok */
static int FS_input_date( DATE * const result, const char * const buf, const size_t size ) {
    struct tm tm ;
    if ( size<2 || buf[0]=='\0' || buf[0]=='\n' ) {
        *result = time(NULL) ;
    } else if (
            strptime(buf,"%a %b %d %T %Y",&tm) == NULL
            && strptime(buf,"%b %d %T %Y",&tm) == NULL
            && strptime(buf,"%c",&tm) == NULL
            && strptime(buf,"%D %T",&tm) == NULL )
    {
        return -EINVAL ;
    } else {
        *result = mktime(&tm) ;
    }
    return 0 ;
}

/* returns 0 if ok */
static int FS_input_yesno_array( int * const results, const char * const buf, const size_t size, const struct parsedname * const pn ) {
    int i ;
    int last = pn->ft->ag->elements - 1 ;
    const char * first ;
    const char * end = buf + size - 1 ;
    const char * next = buf ;
    for ( i=0 ; i<=last ; ++i ) {
        if ( next <= end ) {
            first = next ;
            if ( (next=memchr( first, ',' , (size_t)(first-end+1) )) == NULL ) next = end ;
            if ( FS_input_yesno( &results[i], first, (const size_t)(next-first) ) ) results[i]=0 ;
            ++next ; /* past comma */
        } else { /* assume "no" for absent values */
            results[i] = 0 ;
        }
    }
    return 0 ;
}

/* returns number of valid integers, or negative for error */
static int FS_input_integer_array( int * const results, const char * const buf, const size_t size, const struct parsedname * const pn ) {
    int i ;
    int last = pn->ft->ag->elements - 1 ;
    const char * first ;
    const char * end = buf + size - 1 ;
    const char * next = buf ;
    for ( i=0 ; i<=last ; ++i ) {
        if ( next <= end ) {
            first = next ;
            if ( (next=memchr( first, ',' , (size_t)(first-end+1) )) == NULL ) next = end ;
            if ( FS_input_integer( &results[i], first, (const size_t)(next-first) ) ) results[i]=0 ;
            ++next ; /* past comma */
        } else { /* assume 0 for absent values */
            results[i] = 0 ;
        }
    }
    return 0 ;
}

/* returns 0, or negative for error */
static int FS_input_unsigned_array( unsigned int * const results, const char * const buf, const size_t size, const struct parsedname * const pn ) {
    int i ;
    int last = pn->ft->ag->elements - 1 ;
    const char * first ;
    const char * end = buf + size - 1 ;
    const char * next = buf ;
    for ( i=0 ; i<=last ; ++i ) {
        if ( next <= end ) {
            first = next ;
            if ( (next=memchr( first, ',' , (size_t)(first-end+1) )) == NULL ) next = end ;
            if ( FS_input_unsigned( &results[i], first, (const size_t)(next-first) ) ) results[i]=0 ;
            ++next ; /* past comma */
        } else { /* assume 0 for absent values */
            results[i] = 0 ;
        }
    }
    return 0 ;
}

/* returns 0, or negative for error */
static int FS_input_float_array( FLOAT * const results, const char * const buf, const size_t size, const struct parsedname * const pn ) {
    int i ;
    int last = pn->ft->ag->elements - 1 ;
    const char * first ;
    const char * end = buf + size - 1 ;
    const char * next = buf ;
    for ( i=0 ; i<=last ; ++i ) {
        if ( next <= end ) {
            first = next ;
            if ( (next=memchr( first, ',' , (size_t)(first-end+1) )) == NULL ) next = end ;
            if ( FS_input_float( &results[i], first, (const size_t)(next-first) ) ) results[i]=0. ;
            ++next ; /* past comma */
        } else { /* assume 0. for absent values */
            results[i] = 0. ;
        }
    }
    return 0 ;
}

/* returns 0, or negative for error */
static int FS_input_date_array( DATE * const results, const char * const buf, const size_t size, const struct parsedname * const pn ) {
    int i ;
    int last = pn->ft->ag->elements - 1 ;
    const char * first ;
    const char * end = buf + size - 1 ;
    const char * next = buf ;
    DATE now = time(NULL) ;
    for ( i=0 ; i<=last ; ++i ) {
        if ( next <= end ) {
            first = next ;
            if ( (next=memchr( first, ',' , (size_t)(first-end+1) )) == NULL ) next = end ;
            if ( FS_input_date( &results[i], first, (const size_t)(next-first) ) ) results[i]=now ;
            ++next ; /* past comma */
        } else { /* assume now for absent values */
            results[i] = now ;
        }
    }
    return 0 ;
}
