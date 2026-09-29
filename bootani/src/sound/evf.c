/*************************************************************************
*                                                                        *
* 	Here live the event functions					  *
*                                                                        *
*************************************************************************/
#include	"sos.h"
#include	"protos.h"
#include	"externs.h"
#include	<stdint.h>

#define	c_p 	current_process		/* shorthand */


ushort	* ptr;				/* pointer to cur. voice data	*/


unsigned short	un_int;
uchar			un_char;

int f_MidiNoteOn(void)
{
	return(1);
}
int f_MidiNoteOff(void)
{
	return(1);
}
int f_MidiProgram(void)
{
	return(1);
}
int f_MidiVolume(void)
{
	return(1);
}
int f_MidiTempo(void)
{
	return(1);
}
int f_MidiControl(void)
{
	return(1);
}
int f_MidiEOT(void)
{
	return(1);
}


int f_note(void)
{
	ptr = ++current_process->mem_ptr.i;	/* get past func byte */
	b_value = (uchar) *(ushort *)ptr++; 		/* point to delay */
	if (b_value	& 0x80) {			/* 1 byte duration */
		dp_word1 = *(uchar *) ptr;
		b_value &= 0x7f;
	}
	else {

	dp_word1 = *(unsigned short *)ptr++;

	}
	current_process->function = *(ptr);
	current_process->mem_ptr.i = ptr;
	current_process->timer += dp_word1;
	ti->pitch = value_16_bit = (b_value << 8) + ti->transpose;
	if (current_level == LEVEL_EFFECT) {
		ti->pitch += global_fxpose;
	}
/*		ti->pitch += global_fxpose;*/
		value_16_bit = ti->pitch;

	if (current_level < channel_level[current_channel]) 
	    return(1);
	note_on_functions[current_channel]();
	return(1);
}

int f_gliss(void)
{
#ifdef GLISS
/*#ifdef	BIGOS*/
	uchar	*tmp_ptr;

	tmp_ptr = c_p->mem_ptr.c++;
	if (c_p->type == 0) {		/* first time						*/
		c_p->type = 1;			/* set not first time flag			*/
		c_p->data1 = *c_p->mem_ptr.i++;		/* get base pitch	*/
		c_p->mem_ptr.i++;					/* pass delta		*/
		c_p->data2 = *c_p->mem_ptr.i;		/* get counter		*/
		c_p->mem_ptr.c = tmp_ptr;		
	}
	else {
		if (c_p->data2-- == 0) {
			c_p->mem_ptr.c = tmp_ptr + 7;		/* point to xtra time	*/
			c_p->type = 0;
			c_p->timer += *c_p->mem_ptr.i++;	/* get xtra time*/
			c_p->data1 += *c_p->mem_ptr.i++;		/* get xtra frac	*/
			c_p->function = *c_p->mem_ptr.c;

		}
		else {
			c_p->data1 += *(++c_p->mem_ptr.i);
			c_p->timer += 4;
			c_p->mem_ptr.c = tmp_ptr;			/* point back to us	*/
	    }
		ti->pitch = value_16_bit = c_p->data1 + ti->transpose;
		if (current_level < channel_level[current_channel]) 
		    return(1);
		slur_functions[current_channel]();
	}		
	return(1);
#endif
	return(1);
}

int f_rest(void)
{
/*	unsigned int	del;*/

	ptr = ++current_process->mem_ptr.i;
	un_int  = *(unsigned short *)ptr++;
	current_process->function = *ptr;
	current_process->timer += un_int;
	current_process->mem_ptr.i = ptr;

	if (current_level < channel_level[current_channel]) 
	    return(1);
	note_off_functions[current_channel]();
	return(1);
}



int f_srest(void)
{

	return(1);
}

int f_jumpto(void)
{

/*	uchar	*addr;*/

	current_process->mem_ptr.i++;		/* get past func byte */
	ptr = *(ushort **)current_process->mem_ptr.i++;  /* point to next func */
	current_process->mem_ptr.i = ptr;
	current_process->function = *ptr;	/* get function */
	return(1);
}

int f_loop(void)
{
	ti->loop_counter[ti->loop_level] = *(++c_p->mem_ptr.i);
	ti->loop_addr[(ti->loop_level)++] = (ushort *) ++c_p->mem_ptr.i;
	c_p->function = *c_p->mem_ptr.i;
	return(1);
}

int f_endloop(void)
{
	if (--(ti->loop_counter[ti->loop_level-1]) != 0) {
	  c_p->mem_ptr.i = (ushort *)ti->loop_addr[ti->loop_level-1];
	}
	else {					/* done looping */
	    c_p->mem_ptr.i++;
	    ti->loop_level--;
	}
	c_p->function = *c_p->mem_ptr.i;
	return(1);
}



int f_patch(void)
{
	ushort	pat;

    ptr = ++current_process->mem_ptr.i;		/* get past func byte */
	pat = *(ushort *)ptr++;  /* point to patch # */
	current_process->function = *(ptr);
	current_process->mem_ptr.i = ptr;
	ti->patch = pat;
	ti->pan = ti->volume = 0;

	if (current_level < channel_level[current_channel]) 
	    return(1);
	patch_functions[current_channel](pat);
	return(1);
}

int f_pan(void)
{
      	ptr = ++current_process->mem_ptr.i;		  /* get past func byte */
	a_value = *(uchar *)ptr++; /* point to pan */
	current_process->function = *ptr;
	current_process->mem_ptr.i = ptr;
	ti->pan = a_value;
	if (current_level < channel_level[current_channel]) 
	    return(1);
	pan_functions[current_channel](current_channel,a_value,ti->patch);
	return(1);
}
int f_paninc(void)
{
/*#ifdef	BIGOS */
      	ptr = ++current_process->mem_ptr.i;		  /* get past func byte */
	a_value = *(char *)ptr++; /* point to pan */
	current_process->function = *ptr;
	current_process->mem_ptr.i = ptr;
	ti->pan += (signed char) a_value;
	if (current_level < channel_level[current_channel]) 
	    return(1);
	pan_functions[current_channel](current_channel,ti->pan,ti->patch);
	return(1);
/*#endif*/
}



int f_mux(void)
{
	/* NOTE: legacy 16-bit-pointer event (label stored in one word); it is
	   not used by the boot score and cannot work with wider pointers. */
	ti->mux_addr[(ti->mux_level)++] = (ushort *) (++c_p->mem_ptr.c + 2);
	c_p->mem_ptr.c = (uchar *)(uintptr_t)*(ushort *)(c_p->mem_ptr.c);
	c_p->function = *c_p->mem_ptr.c;
	return(1);
}
int f_demux(void)
{
/*	uchar	tmp;*/

	un_char = --(ti->mux_level);
	c_p->mem_ptr.c = (uchar *)ti->mux_addr[un_char];
	c_p->function = *c_p->mem_ptr.c;
	return(1);
}

int f_volume(void)
{
	un_char = ti->volume + *(short *)++current_process->mem_ptr.i; /* point to vol */
	current_process->function = *++current_process->mem_ptr.i;
	ti->volume = un_char;
	if (current_level < channel_level[current_channel]) 
	    return(1);
	volume_functions[current_channel](un_char,current_level,ti->patch,current_channel);
	return(1);
}



int f_fxset(void)
{
	c_p->mem_ptr.c++;
	global_fxpose = *c_p->mem_ptr.i++;
	c_p->function = *c_p->mem_ptr.c;
	return(1);
}


int f_xpose(void)
{
	c_p->mem_ptr.i++;
	ti->transpose += *c_p->mem_ptr.i++;
	c_p->function = *c_p->mem_ptr.i;
	return(1);
}

int f_xset(void)
{
	c_p->mem_ptr.i++;
	ti->transpose = *c_p->mem_ptr.i++;
	c_p->function = *c_p->mem_ptr.i;
	return(1);
}


int f_filterinc(void)
{
	c_p->mem_ptr.i++;
	ti->filtercutoff += *c_p->mem_ptr.i++;
	ti->filterres = *c_p->mem_ptr.i++;
	c_p->function = *c_p->mem_ptr.i;
	filter_functions[current_channel]();
	return(1);
}

int f_filterset(void)
{
	c_p->mem_ptr.i++;
	ti->filtercutoff = *c_p->mem_ptr.i++;
	ti->filterres = *c_p->mem_ptr.i++;
	c_p->function = *c_p->mem_ptr.i;
	filter_functions[current_channel]();
	return(1);
}


int f_slur(void)
{
/*	unsigned int	del;*/

	ptr = ++current_process->mem_ptr.i;	/* get past func byte */
	b_value = (uchar) *(ushort *)ptr++; 		/* point to delay */
	if (b_value	& 0x80) {			/* 1 byte duration */
		dp_word1 = *(uchar *) ptr;
		b_value &= 0x7f;
	}
	else {

	dp_word1 = *(unsigned short *)ptr++;

	}
	current_process->function = *(ptr);
	current_process->mem_ptr.i = ptr;
	current_process->timer += dp_word1;
	ti->pitch = value_16_bit = (b_value << 8) + ti->transpose;
	if (current_level == LEVEL_EFFECT) {
		ti->pitch += global_fxpose;
	}
/*		ti->pitch += global_fxpose;*/
		value_16_bit = ti->pitch;

	if (current_level < channel_level[current_channel]) 
	    return(1);
	slur_functions[current_channel]();
	return(1);
	//

}


int f_ring(void)
{

	ptr = ++current_process->mem_ptr.i;
	un_int = *(unsigned short *)ptr++;
	current_process->function = *ptr;
	current_process->timer += un_int;
	current_process->mem_ptr.i = ptr;
	return(1);
}


int f_clockset(void)
{
	current_process->mem_ptr.c++;		/* get past func byte */
	clock_cntr_value[c_p->level] = *(uchar *)current_process->mem_ptr.c++;/* point to next func */
	current_process->function = *current_process->mem_ptr.c;
	return(1);
}

int f_clockinc(void)
{
	return(1);
}


int f_musicclockinc(void)
{
	return(1);
}



/*************************************************************************
*                                                                        *
* 	Used to end a track of music or a sound effect			  *
*                                                                        *
*************************************************************************/

int f_end(void)
{
/*	un_char = c_p->hard_channel;*/
//	nosound(current_channel);
//	ti = get_music_info_block();
//	remove_processes_by_level_and_channel(LEVEL_EFFECT,current_channel);
//	channel_level[current_channel] = LEVEL_MUSIC;
//	current_level = LEVEL_MUSIC;
//	if (ti->patch == NULL)
//		return(0);
//	ptr = ti->patch;
//	patch_functions[current_channel](ptr);
//	pan_functions[current_channel](current_channel,ti->pan,ptr);
//	volume_functions[current_channel](ti->volume,current_level,ptr,current_channel);
	return(0);
}


	







int f_mark(void)
{
#ifdef	BIGOS
	uchar	m_value;
	uchar	dest;
	
	c_p->mem_ptr.c++;
	m_value = *c_p->mem_ptr.c++;	/* get marker # */
	dest =  *c_p->mem_ptr.c++;	/* get destination */
	c_p->function = *c_p->mem_ptr.c;
	if (m_value == marker) {
		DINT;
		put_fifo(dest);
		EINT;
		marker = 0;
	}
#endif
	return(1);
}


int f_sound_call(void)
{
/*#ifdef	BIGOS*/
	uchar	dest;
	
	c_p->mem_ptr.c++;
	dest =  *c_p->mem_ptr.c++;	/* get destination */
	c_p->function = *c_p->mem_ptr.c;
/*	DINT;*/
	put_fifo(dest);
/*	EINT;*/
	return(1);
/*#endif*/
}

int f_intvarset(void)
{
#ifdef	BIGOS
	uchar	index;
	c_p->mem_ptr.c++;
	index = *c_p->mem_ptr.c++;
	int_vars[index] = *(ushort *)c_p->mem_ptr.i++;
	c_p->function = *c_p->mem_ptr.c;
	return(1);
#endif
	return(1);
}
int f_intvarinc(void)
{
#ifdef	BIGOS
	uchar	index;
	c_p->mem_ptr.c++;
	index = *c_p->mem_ptr.c++;
	int_vars[index] += *(char *)c_p->mem_ptr.c++;
	c_p->function = *c_p->mem_ptr.c;
	return(1);
#endif
	return(1);
}



int f_user_1_var_evf(void)
{
/*#ifdef	BIGOS*/

	a_value = *(++c_p->mem_ptr.c);		/* get parameter in a_value */
	c_p->function = *++c_p->mem_ptr.c;
	user_1_var_evf_functions[current_channel]();
	return(1);
/*#endif*/
}
int f_user_2_var_evf(void)
{
	a_value = *(++c_p->mem_ptr.c);		/* get parameter in a_value */
	b_value = *(++c_p->mem_ptr.c);		/* get 2nd parameter in b_value */
	c_p->function = *++c_p->mem_ptr.c;
	user_2_var_evf_functions[current_channel]();
	return(1);

}


int f_sig(void)
{
	a_value = *(++c_p->mem_ptr.c);		/* get parameter in a_value */
	c_p->function = *++c_p->mem_ptr.c;
//	signal_function();
	return(1);
}
