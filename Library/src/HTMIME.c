/*			MIME Message Parse			HTMIME.c
**			==================
**
**	This is RFC 1341-specific code.
**	The input stream pushed into this parser is assumed to be
**	stripped on CRs, ie lines end with LF, not CR LF.
**	(It is easy to change this except for the body part where
**	conversion can be slow.)
**
** History:
**	   Feb 92	Written Tim Berners-Lee, CERN
**	 8 Jul 94  FM	Insulate free() from _free structure element.
**
*/
#include "HTFormat.h"
#include "HTAlert.h"
#include "HTFWriter.h"
#include "HTMIME.h"					 /* Implemented here */

#define VALUE_SIZE 128		/* @@@@@@@ Arbitrary? */

/*		MIME Object
**		-----------
*/

typedef enum _MIME_state {
    MIME_TRANSPARENT,	/* put straight through to target ASAP! */
    BEGINNING_OF_LINE,
    CONTENT_T,
    CONTENT_TRANSFER_ENCODING,
    CONTENT_TYPE,
    AA,
    AUTHENTICATE,
    PROTECTION,
    LOCATION,
    SKIP_GET_VALUE,		/* Skip space then get value */
    GET_VALUE,		/* Get value till white space */
    JUNK_LINE,		/* Ignore the rest of this folded line */
    NEWLINE,		/* Just found a LF .. maybe continuation */
    CHECK			/* check against check_pointer */
} MIME_state;

struct _HTStream {
	CONST HTStreamClass *	isa;
	
	BOOL			net_ascii;	/* Is input net ascii? */
	MIME_state		state;		/* current state */
	MIME_state		if_ok;		/* got this state if match */
	MIME_state		field;		/* remember which field */
	MIME_state		fold_state;	/* state on a fold */
	CONST char *		check_pointer;	/* checking input */
	
	char *			value_pointer;	/* storing values */
	char 			value[VALUE_SIZE];
	int			value_num;	/* What token are we reading */
	
	HTStream *		sink;		/* Given on creation */
	HTRequest *		request;	/* Given on creation */
	
	char *			boundary;	/* For multipart */
	
	HTFormat		encoding;	/* Content-Transfer-Encoding */
	HTFormat		format;		/* Content-Type */
	HTStream *		target;		/* While writing out */
	HTAtom *		targetRep;	/* Converting into? */
};


/*_________________________________________________________________________
**
**			A C T I O N 	R O U T I N E S
*/

/*	Character handling
**	------------------
**
**	This is a FSM parser which is tolerant as it can be of all
**	syntax errors.  It ignores field names it does not understand,
**	and resynchronises on line beginnings.
*/

PRIVATE void HTMIME_put_character ARGS2(HTStream *, me, char, c)
{
    /* This slightly simple conversion just strips CR and turns LF to
    ** newline. On unix LF is \n but on Mac \n is CR for example.
    ** See NetToText for an implementation which preserves single CR or LF.
    */
    if (me->net_ascii) {
        c = FROMASCII(c);
	if (c == CR) return;
	else if (c == LF) c = '\n';
    }
    
    switch(me->state) {

    case MIME_TRANSPARENT:
        (*me->target->isa->put_character)(me->target, c);
	break;

    case NEWLINE:
	if (c != '\n' && WHITE(c)) {		/* Folded line */
	    me->state = me->fold_state;	/* pop state before newline */
	    break;
	}
	me->value_num = 0;
	
	/*	else Falls through */
	
    case BEGINNING_OF_LINE:
        switch(c) {
	  case 'c':
	  case 'C':
	    me->check_pointer = "ontent-t";
	    me->if_ok = CONTENT_T;
	    me->state = CHECK;
	    break;

	  case 'l':
	  case 'L':
	    me->check_pointer = "ocation:";
	    me->if_ok = LOCATION;
	    me->state = CHECK;
	    break;

	  case 'u':
	  case 'U':
	    me->check_pointer = "ri:";
	    me->if_ok = LOCATION;
	    me->state = CHECK;
	    break;

	  case 'w':
	  case 'W':
	    me->check_pointer = "ww-";
	    me->if_ok = AA;
	    me->state = CHECK;
	    break;

	  case '\n':			/* Blank line: End of Header! */
	    {
	        if (TRACE) fprintf(stderr,
			"HTMIME: MIME content type is %s, converting to %s\n",
			HTAtom_name(me->format), HTAtom_name(me->targetRep));
		me->target = HTStreamStack(me->format, me->targetRep,
					   me->sink, me->request, NO);
		if (me->target) {
		    me->state = MIME_TRANSPARENT;
		} else {
		    if (TRACE)
			fprintf(stderr, "MIMEParser.. Can't convert to output format\n");
		    me->target = me->sink;	/* Cheat */
		}
	    }
	    break;
	    
	default:
	   goto bad_field_name;
	   break;
	   
	} /* switch on character */
        break;
	
    case CHECK:				/* Check against string */
        if (TOLOWER(c) == *(me->check_pointer)++) {
	    if (!*me->check_pointer) me->state = me->if_ok;
	} else {		/* Error */
	    if (TRACE) fprintf(stderr,
	    	"HTMIME: Bad character `%c' found where `%s' expected\n",
		c, me->check_pointer - 1);
	    goto bad_field_name;
	}
	break;
	
    case CONTENT_T:
        switch(c) {
	case 'r':
	case 'R':
	    me->check_pointer = "ansfer-encoding:";
	    me->if_ok = CONTENT_TRANSFER_ENCODING;
	    me->state = CHECK;
	    break;
	    
	case 'y':
	case 'Y':
	    me->check_pointer = "pe:";
	    me->if_ok = CONTENT_TYPE;
	    me->state = CHECK;
	    break;
	    
	default:
	    goto bad_field_name;
	    
	} /* switch on character */
	break;

    case AA:
	switch(c) {
	  case 'a':
	  case 'A':
	    me->check_pointer = "uthenticate:";
	    me->if_ok = AUTHENTICATE;
	    me->state = CHECK;
	    break;

	  case 'p':
	  case 'P':
	    me->check_pointer = "rotection-template:";
	    me->if_ok = PROTECTION;
	    me->state = CHECK;
	    break;

	  default:
	    goto bad_field_name;
	}	    
	break;

    case AUTHENTICATE:
        me->field = me->state;		/* remember it */
	me->value_pointer = me->value;
	me->state = GET_VALUE;
	break;

    case CONTENT_TYPE:
    case CONTENT_TRANSFER_ENCODING:
    case LOCATION:
    case PROTECTION:
        me->field = me->state;		/* remember it */
	me->state = SKIP_GET_VALUE;

				/* Fall through! */
    case SKIP_GET_VALUE:
    	if (c == '\n') {
	   me->fold_state = me->state;
	   me->state = NEWLINE;
	   break;
	}
	if (WHITE(c)) break;	/* Skip white space */
	me->value_pointer = me->value;
	me->state = GET_VALUE;   
	/* Fall through to store first character */
	
    case GET_VALUE:
    	if (WHITE(c)) {			/* End of field */
	    *me->value_pointer = 0;
	    me->value_num++;
	    if (!*me->value)			/* Ignore empty field */
		break;
	    switch (me->field) {
	    case CONTENT_TYPE:
	        me->format = HTAtom_for(me->value);
		break;
	    case CONTENT_TRANSFER_ENCODING:
	        me->encoding = HTAtom_for(me->value);
		break;
            case LOCATION:
		StrAllocCopy(me->request->redirect, me->value);
		break;
            case AUTHENTICATE:
		if (me->value_num == 1) {
		    StrAllocCopy(me->request->WWWAAScheme, me->value);
		    me->value_pointer = me->value;
		} else if (me->value_num == 2) {
		    StrAllocCopy(me->request->WWWAARealm, me->value);
		}
		break;
            case PROTECTION:
		StrAllocCopy(me->request->WWWprotection, me->value);
		break;
	    default:		/* Should never get here */
	    	break;
	    }
	} else {
	    if (me->value_pointer < me->value + VALUE_SIZE - 1) {
	        *me->value_pointer++ = c;
		break;
	    } else {
	        goto value_too_long;
	    }
	}
	/* Fall through */
	
    case JUNK_LINE:
        if (c == '\n') {
	    me->state = NEWLINE;
	    me->fold_state = me->state;
	}
	break;
	
	
    } /* switch on state*/
    
    return;
    
value_too_long:
    if (TRACE) fprintf(stderr,
    	"HTMIME: *** Syntax error. (string too long)\n");
    
bad_field_name:				/* Ignore it */
    me->state = JUNK_LINE;
    return;
    
}



/*	String handling
**	---------------
**
**	Strings must be smaller than this buffer size.
*/
PRIVATE void HTMIME_put_string ARGS2(HTStream *, me, CONST char*, s)
{
    while (me->state != MIME_TRANSPARENT && *s)
	HTMIME_put_character(me, *s++);
    if (*s)
        (*me->target->isa->put_string)(me->target, s);
}


/*	Buffer write.  Buffers can (and should!) be big.
**	------------
*/
PRIVATE void HTMIME_write ARGS3(HTStream *, me, CONST char *, b, int, l)
{
    while (me->state != MIME_TRANSPARENT && l-- > 0)
	HTMIME_put_character(me, *b++);
    if (l > 0)
        (*me->target->isa->put_block)(me->target, b, l);
}


/*	Free an HTML object
**	-------------------
**
*/
PRIVATE int HTMIME_free ARGS1(HTStream *, me)
{
    if (me->target) (*me->target->isa->_free)(me->target);
    free(me);
    return 0;
}

/*	End writing
*/

PRIVATE int HTMIME_abort ARGS2(HTStream *, me, HTError, e)
{
    if (me->target) (*me->target->isa->abort)(me->target, e);
    free(me);
    return EOF;
}



/*	Structured Object Class
**	-----------------------
*/
PRIVATE CONST HTStreamClass HTMIME =
{		
	"MIMEParser",
	HTMIME_free,
	HTMIME_abort,
	HTMIME_put_character,
	HTMIME_put_string,
	HTMIME_write
}; 


/*	Subclass-specific Methods
**	-------------------------
*/

PUBLIC HTStream* HTMIMEConvert ARGS5(
	HTRequest *,		request,
	void *,			param,
	HTFormat,		input_format,
	HTFormat,		output_format,
	HTStream *,		output_stream)
{
    HTStream* me;
    
    me = (HTStream*)calloc(1, sizeof(*me));
    if (me == NULL) outofmem(__FILE__, "HTMIMEConvert");
    me->isa = &HTMIME;       

    me->sink = 		output_stream;
    me->request = 	request;
    me->state = 	BEGINNING_OF_LINE;
    me->format = 	WWW_PLAINTEXT;
    me->targetRep = 	output_format;
    return me;
}

PUBLIC HTStream* HTNetMIME ARGS5(
	HTRequest *,		request,
	void *,			param,
	HTFormat,		input_format,
	HTFormat,		output_format,
	HTStream *,		output_stream)
{
    HTStream* me = HTMIMEConvert(
    	request, param, input_format, output_format, output_stream);
    if (!me) return NULL;
    
    me->net_ascii = YES;
    return me;
}


