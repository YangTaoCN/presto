#include "presto.h"
#include "mask.h"
#include "wapp.h"
#include "srfftw.h"

/* All of the following have an _st to indicate static */
static long long numpts_st[MAXPATCHFILES], padpts_st[MAXPATCHFILES], N_st;
static long long filedatalen_st[MAXPATCHFILES];
static int numblks_st[MAXPATCHFILES], corr_level_st, decreasing_freqs_st=0;
static int bytesperpt_st, bytesperblk_st, bits_per_samp_st, numifs_st;
static int numchan_st, numifs_st, ptsperblk_st=WAPP_PTSPERBLOCK;
static int need_byteswap_st, sampperblk_st;
static double times_st[MAXPATCHFILES], mjds_st[MAXPATCHFILES];
static double elapsed_st[MAXPATCHFILES], T_st, dt_st, dtus_st;
static double startblk_st[MAXPATCHFILES], endblk_st[MAXPATCHFILES];
static double scale_min_st=0.0, scale_max_st=3.0;
static double corr_rate_st, corr_scale_st;
static infodata idata_st[MAXPATCHFILES];
static unsigned char databuffer[2*WAPP_MAXDATLEN], padval=128;
static unsigned char lagbuffer[WAPP_MAXLAGLEN];
static int currentfile, currentblock;
static int bufferpts=0, padnum=0, shiftbuffer=1;
static fftw_plan fftplan;

double slaCldj(int iy, int im, int id, int *j);
static double inv_cerf(double input);
static void vanvleck3lev(float *rho, int npts);
static void vanvleck9lev(float *rho, int npts);

int check_WAPP_byteswap(WAPP_HEADER *hdr)
{
  int ii;
  
  if ((hdr->header_size != 2048) &&
      (hdr->nifs < 1 || hdr->nifs > 4)){
    hdr->src_ra = swap_double(hdr->src_ra);
    hdr->src_dec = swap_double(hdr->src_dec);
    hdr->start_az = swap_double(hdr->start_az);
    hdr->start_za = swap_double(hdr->start_za);
    hdr->start_ast = swap_double(hdr->start_ast);
    hdr->start_lst = swap_double(hdr->start_lst);
    hdr->cent_freq = swap_double(hdr->cent_freq);
    hdr->obs_time = swap_double(hdr->obs_time);
    hdr->samp_time = swap_double(hdr->samp_time);
    hdr->wapp_time = swap_double(hdr->wapp_time);
    hdr->bandwidth = swap_double(hdr->bandwidth);
    hdr->power_analog[0] = swap_double(hdr->power_analog[0]);
    hdr->power_analog[1] = swap_double(hdr->power_analog[1]);
    hdr->psr_dm = swap_double(hdr->psr_dm);
    hdr->num_lags = swap_int(hdr->num_lags);
    hdr->scan_number = swap_int(hdr->scan_number);
    hdr->header_version = swap_int(hdr->header_version);
    hdr->header_size = swap_int(hdr->header_size);    
    hdr->nifs = swap_int(hdr->nifs);
    hdr->level = swap_int(hdr->level);
    hdr->sum = swap_int(hdr->sum);
    hdr->freqinversion = swap_int(hdr->freqinversion);
    hdr->lagformat = swap_int(hdr->lagformat);
    hdr->lagtrunc = swap_int(hdr->lagtrunc);
    hdr->timeoff = swap_longlong(hdr->timeoff);
    for (ii=0; ii<9; ii++){
      hdr->rphase[ii] = swap_double(hdr->rphase[ii]);
      hdr->psr_f0[ii] = swap_double(hdr->psr_f0[ii]);
      hdr->poly_tmid[ii] = swap_double(hdr->poly_tmid[ii]);
      hdr->num_coeffs[ii] = swap_int(hdr->num_coeffs[ii]);
    }
    for (ii=0; ii<144; ii++)
      hdr->coeff[ii] = swap_double(hdr->coeff[ii]);
    return 1;
  } else {
    return 0;
  }
}


static double UT_strings_to_MJD(char *obs_date, char *start_time, 
				int *mjd_day, double *mjd_fracday)
{
  int year, month, day, hour, min, sec, err;

  sscanf(obs_date, "%4d%2d%2d", &year, &month, &day);
  sscanf(start_time, "%2d:%2d:%2d", &hour, &min, &sec);
  *mjd_fracday = (hour + (min + (sec / 60.0)) / 60.0) / 24.0;
  *mjd_day = slaCldj(year, month, day, &err);
  return *mjd_day + *mjd_fracday;
}

double wappcorrect(double mjd)
/* subroutine to return correction to wapp_time (us) based on mjd */
{
  double correction;
  
  /* assume no correction initially */
  correction=0.0;
  
  if ( (mjd >= 51829.0) && (mjd < 51834.0) ) correction=-0.08;
  if ( (mjd >= 51834.0) && (mjd < 51854.0) ) correction=-0.68;
  if ( (mjd >= 51854.0) && (mjd < 51969.0) ) correction=+0.04;  

  if (correction != 0.0) {
    fprintf(stderr,"WARNING: correction %f us applied for MJD %.1f\n",
            correction,mjd);
    fflush(stderr);
  }
  
  return(correction);
}

void WAPP_hdr_to_inf(WAPP_HEADER *hdr, infodata *idata)
/* Convert WAPP header into an infodata structure */
{
  double MJD;
  char ctmp[80];

  strncpy(idata->object, hdr->src_name, 24);
  idata->ra_h = (int) floor(hdr->src_ra / 10000.0);
  idata->ra_m = (int) floor((hdr->src_ra - 
			     idata->ra_h * 10000) / 100.0);
  idata->ra_s = hdr->src_ra - idata->ra_h * 10000 - 
    idata->ra_m * 100;
  idata->dec_d = (int) floor(fabs(hdr->src_dec) / 10000.0);
  idata->dec_m = (int) floor((fabs(hdr->src_dec) - 
			      idata->dec_d * 10000) / 100.0);
  idata->dec_s = fabs(hdr->src_dec) - idata->dec_d * 10000 - 
    idata->dec_m * 100;
  if (hdr->src_dec < 0.0)
    idata->dec_d = -idata->dec_d;
  strcpy(idata->telescope, "Arecibo");
  strcpy(idata->instrument, "WAPP");
  idata->num_chan = hdr->num_lags;
  MJD = UT_strings_to_MJD(hdr->obs_date, hdr->start_time, 
			  &(idata->mjd_i), &(idata->mjd_f));
  idata->dt = (wappcorrect(MJD) + hdr->wapp_time) / 1000000.0;
  idata->N = hdr->obs_time / idata->dt;
  idata->freqband = hdr->bandwidth;
  idata->chan_wid = fabs(idata->freqband / idata->num_chan);
  idata->freq = hdr->cent_freq - 0.5*idata->freqband + 0.5*idata->chan_wid;
  idata->fov = 1.2 * SOL * 3600.0 / (1000000.0 * idata->freq * 300.0 * DEGTORAD);
  idata->bary = 0;
  idata->numonoff = 0;
  strcpy(idata->band, "Radio");
  strcpy(idata->analyzer, "Scott Ransom");
  strncpy(idata->observer, hdr->observers, 24);
  if (hdr->sum)
    sprintf(ctmp, 
	    "%d %d-level IF(s) were summed.  Lags are %d bit ints.", 
	    numifs_st, corr_level_st, bits_per_samp_st);
  else
    sprintf(ctmp, "%d %d-level IF(s) were not summed.  Lags are %d bit ints.", 
	    numifs_st, corr_level_st, bits_per_samp_st);
  sprintf(idata->notes, "Starting Azimuth (deg) = %.15g,  Zenith angle (deg) = %.15g\n    Project ID %s, Scan number %ld, Date: %s %s.\n    %s\n", 
	  hdr->start_az, hdr->start_za, hdr->project_id, hdr->scan_number, 
	  hdr->obs_date, hdr->start_time, ctmp);
}


void get_WAPP_file_info(FILE *files[], int numfiles, long long *N, 
		       int *ptsperblock, int *numchan, double *dt, 
		       double *T, infodata *idata, int output)
/* Read basic information into static variables and make padding      */
/* calculations for a set of WAPP rawfiles that you want to patch      */
/* together.  N, numchan, dt, and T are return values and include all */
/* the files with the required padding.  If output is true, prints    */
/* a table showing a summary of the values.                           */
{
  int ii, asciihdrlen=1;
  char cc=1;
  WAPP_HEADER hdr;

  if (numfiles > MAXPATCHFILES){
    printf("\nThe number of input files (%d) is greater than \n", numfiles);
    printf("   MAXPATCHFILES=%d.  Exiting.\n\n", MAXPATCHFILES);
    exit(0);
  }
  /* Skip the ASCII header file */
  while((cc=fgetc(files[0]))!='\0')
    asciihdrlen++;
  /* Read the binary header (no byte-swapping capabilities yet) */
  chkfread(&hdr, WAPP_HEADER_SIZE, 1, files[0]);
  /* See if we need to byte-swap and if so, doit */
  need_byteswap_st = check_WAPP_byteswap(&hdr);
  numifs_st = hdr.nifs;
  if (numifs_st > 1)
    printf("\nNumber of IFs (%d) is > 1!  I can't handle this yet!\n\n",
	   numifs_st);
  if (hdr.freqinversion)
    decreasing_freqs_st = 1;
  if (hdr.level==1)
    corr_level_st = 3;
  else if (hdr.level==2)
    corr_level_st = 9;
  else
    printf("\nError:  Unrecognized level setting!\n\n");
  if (hdr.lagformat==0)
    bits_per_samp_st = 16;
  else if (hdr.lagformat==1)
    bits_per_samp_st = 32;
  else
    printf("\nError:  Unrecognized number of bits per sample!\n\n");
  WAPP_hdr_to_inf(&hdr, &idata_st[0]);
  WAPP_hdr_to_inf(&hdr, idata);
  *numchan = numchan_st = idata_st[0].num_chan;
  fftplan = rfftw_create_plan(2 * numchan_st, 
			      FFTW_REAL_TO_COMPLEX, 
			      FFTW_MEASURE);
  *ptsperblock = ptsperblk_st;
  sampperblk_st = ptsperblk_st * numchan_st;
  bytesperpt_st = (numchan_st * numifs_st * bits_per_samp_st) / 8;
  bytesperblk_st = ptsperblk_st * bytesperpt_st;
  filedatalen_st[0] = chkfilelen(files[0], 1) - 
    asciihdrlen - WAPP_HEADER_SIZE;
  numblks_st[0] = filedatalen_st[0] / bytesperblk_st;
  numpts_st[0] = numblks_st[0] * ptsperblk_st;
  N_st = numpts_st[0];
  dtus_st = idata_st[0].dt * 1000000.0;
  corr_rate_st = 1.0 / (dtus_st - WAPP_DEADTIME);
  corr_scale_st = corr_rate_st / idata->freqband;
  if (corr_level_st==9) /* 9-level sampling */
    corr_scale_st /= 16.0;
  if (hdr.sum) /* summed IFs (search mode) */
    corr_scale_st /= 2.0;
  dt_st = *dt = idata_st[0].dt;
  times_st[0] = numpts_st[0] * dt_st;
  mjds_st[0] = idata_st[0].mjd_i + idata_st[0].mjd_f;
  elapsed_st[0] = 0.0;
  startblk_st[0] = 1;
  endblk_st[0] = (double) numpts_st[0] / ptsperblk_st;
  padpts_st[0] = padpts_st[numfiles-1] = 0;
  for (ii=1; ii<numfiles; ii++){
    /* Skip the ASCII header file */
    cc=1;
    while((cc=fgetc(files[ii]))!='\0');
    chkfread(&hdr, WAPP_HEADER_SIZE, 1, files[ii]);
    /* See if we need to byte-swap and if so, doit */
    need_byteswap_st = check_WAPP_byteswap(&hdr);
    WAPP_hdr_to_inf(&hdr, &idata_st[ii]);
    if (idata_st[ii].num_chan != numchan_st){
      printf("Number of channels (file %d) is not the same!\n\n", ii+1);
    }
    if (idata_st[ii].dt != dt_st){
      printf("Sample time (file %d) is not the same!\n\n", ii+1);
    }
    filedatalen_st[ii] = chkfilelen(files[ii], 1) - 
      asciihdrlen - WAPP_HEADER_SIZE;
    numblks_st[ii] = filedatalen_st[ii] / bytesperblk_st;
    numpts_st[ii] = numblks_st[ii] * ptsperblk_st;
    times_st[ii] = numpts_st[ii] * dt_st;
    /* If the MJDs are equal, then this is a continuation */
    /* file.  In that case, calculate the _real_ time     */
    /* length of the previous file and add it to the      */
    /* previous files MJD to get the current MJD.         */
    mjds_st[ii] = idata_st[ii].mjd_i + idata_st[ii].mjd_f;
    if (mjds_st[ii]==mjds_st[0]){
      elapsed_st[ii] = (filedatalen_st[ii-1] / bytesperpt_st) * dt_st;
      idata_st[ii].mjd_f = idata_st[ii-1].mjd_f + elapsed_st[ii] / SECPERDAY;
      idata_st[ii].mjd_i = idata_st[ii-1].mjd_i;
      if (idata_st[ii].mjd_f >= 1.0){
	idata_st[ii].mjd_f -= 1.0;
	idata_st[ii].mjd_i++;
      }
      mjds_st[ii] = idata_st[ii].mjd_i + idata_st[ii].mjd_f;
    } else {
      elapsed_st[ii] = mjd_sec_diff(idata_st[ii].mjd_i, idata_st[ii].mjd_f,
				    idata_st[ii-1].mjd_i, idata_st[ii-1].mjd_f);
    }
    padpts_st[ii-1] = (long long)((elapsed_st[ii]-times_st[ii-1]) / 
				  dt_st + 0.5);
    elapsed_st[ii] += elapsed_st[ii-1];
    N_st += numpts_st[ii] + padpts_st[ii-1];
    startblk_st[ii] = (double) (N_st - numpts_st[ii]) /
      ptsperblk_st + 1;
    endblk_st[ii] = (double) (N_st) / ptsperblk_st;
  }
  padpts_st[numfiles-1] = ((long long) ceil(endblk_st[numfiles-1]) *
                           ptsperblk_st - N_st);
  N_st += padpts_st[numfiles-1];
  *N = N_st;
  *T = T_st = N_st * dt_st;
  currentfile = currentblock = 0;
  if (output){
    printf("   Number of files = %d\n", numfiles);
    printf("      Points/block = %d\n", ptsperblk_st);
    printf("   Num of channels = %d\n", numchan_st);
    printf("  Total points (N) = %lld\n", N_st);
    printf("  Sample time (dt) = %-14.14g\n", dt_st);
    printf("    Total time (s) = %-14.14g\n", T_st);
    printf("  ASCII Header (B) = %d\n", asciihdrlen);
    printf(" Binary Header (B) = %ld\n\n", hdr.header_size);
    printf("File  Start Block    Last Block     Points      Elapsed (s)      Time (s)            MJD           Padding\n");
    printf("----  ------------  ------------  ----------  --------------  --------------  ------------------  ----------\n");
    for (ii=0; ii<numfiles; ii++)
      printf("%2d    %12.11g  %12.11g  %10lld  %14.13g  %14.13g  %17.12f  %10lld\n",
             ii+1, startblk_st[ii], endblk_st[ii], numpts_st[ii],
             elapsed_st[ii], times_st[ii], mjds_st[ii], padpts_st[ii]);
    printf("\n");
  }
}


void WAPP_update_infodata(int numfiles, infodata *idata)
/* Update the onoff bins section in case we used multiple files */
{
  
  int ii, index=2;

  idata->N = N_st;
  if (numfiles==1 && padpts_st[0]==0){
    idata->numonoff = 0;
    return;
  }
  /* Determine the topocentric onoff bins */
  idata->numonoff = 1;
  idata->onoff[0] = 0.0;
  idata->onoff[1] = numpts_st[0] - 1.0;
  for (ii=1; ii<numfiles; ii++){
    if (padpts_st[ii-1]){
      idata->onoff[index] = idata->onoff[index-1] + padpts_st[ii-1];
      idata->onoff[index+1] = idata->onoff[index] + numpts_st[ii];
      idata->numonoff++;
      index += 2;
    } else {
      idata->onoff[index-1] += numpts_st[ii];
    }
  }
  if (padpts_st[numfiles-1]){
    idata->onoff[index] = idata->onoff[index-1] + padpts_st[numfiles-1];
    idata->onoff[index+1] = idata->onoff[index];
    idata->numonoff++;
  }
}


int skip_to_WAPP_rec(FILE *infiles[], int numfiles, int rec)
/* This routine skips to the record 'rec' in the input files   */
/* *infiles.  *infiles contain data from the WAPP at Arecibo   */
/* Returns the record skipped to.                              */
{
  double floor_blk;
  int filenum=0;
 
  if (rec < startblk_st[0])
    rec += (startblk_st[0] - 1);
  if (rec > 0 && rec < endblk_st[numfiles-1]){
 
    /* Find which file we need */
    while (rec > endblk_st[filenum])
      filenum++;
 
    currentblock = rec - 1;
    shiftbuffer = 1;
    floor_blk = floor(startblk_st[filenum]);
 
    /* Set the data buffer to all padding just in case */
    memset(databuffer, padval, 2*WAPP_MAXDATLEN);
 
    /* Warning:  I'm not sure if the following is correct. */
    /*   If really needs accurate testing to see if my     */
    /*   offsets are correct.  Bottom line, don't trust    */
    /*   a TOA determined using the following!             */
 
    if (rec < startblk_st[filenum]){  /* Padding region */
      currentfile = filenum-1;
      chkfileseek(infiles[currentfile], 0, 1, SEEK_END);
      bufferpts = padpts_st[currentfile] % ptsperblk_st;
      padnum = ptsperblk_st * (rec - endblk_st[currentfile] - 1);
      /*
      printf("Padding:  currentfile = %d  bufferpts = %d  padnum = %d\n",
             currentfile, bufferpts, padnum);
      */
    } else {  /* Data region */
      currentfile = filenum;
      chkfileseek(infiles[currentfile], rec - startblk_st[filenum],
                  bytesperblk_st, SEEK_CUR);
      bufferpts = (int)((startblk_st[filenum] - floor_blk) * 
			ptsperblk_st + 0.5);
      padnum = 0;
      /*
      printf("Data:  currentfile = %d  bufferpts = %d  padnum = %d\n",
             currentfile, bufferpts, padnum);
      */
    }
 
  } else {
    printf("\n rec = %d out of range in skip_to_WAPP_rec()\n", rec);
    exit(1);
  }
  return rec;
}


void print_WAPP_hdr(WAPP_HEADER *hdr)
/* Output a WAPP header in human readable form */
{
  int mjd_i;
  double mjd_d;

  printf("\n             Header version = %ld\n", hdr->header_version);
  printf("        Header size (bytes) = %ld\n", hdr->header_size);
  printf("                Source Name = %s\n", hdr->src_name);
  printf(" Observation Date (YYYMMDD) = %s\n", hdr->obs_date);
  printf("    Obs Start UT (HH:MM:SS) = %s\n", hdr->start_time);
  printf("             MJD start time = %.12f\n", 
	 UT_strings_to_MJD(hdr->obs_date, hdr->start_time, &mjd_i, &mjd_d));
  printf("                 Project ID = %s\n", hdr->project_id);
  printf("                  Observers = %s\n", hdr->observers);
  printf("                Scan Number = %ld\n", hdr->scan_number);
  printf("    RA (J2000, HHMMSS.SSSS) = %.4f\n", hdr->src_ra);
  printf("   DEC (J2000, DDMMSS.SSSS) = %.4f\n", hdr->src_dec);
  printf("        Start Azimuth (deg) = %-17.15g\n", hdr->start_az);
  printf("     Start Zenith Ang (deg) = %-17.15g\n", hdr->start_za);
  printf("            Start AST (sec) = %-17.15g\n", hdr->start_ast);
  printf("            Start LST (sec) = %-17.15g\n", hdr->start_lst);
  printf("           Obs Length (sec) = %-17.15g\n", hdr->obs_time);
  printf("      Requested T_samp (us) = %-17.15g\n", hdr->samp_time);
  printf("         Actual T_samp (us) = %-17.15g\n", hdr->wapp_time);
  printf("         Central freq (MHz) = %-17.15g\n", hdr->cent_freq);
  printf("      Total Bandwidth (MHz) = %-17.15g\n", hdr->bandwidth);
  printf("             Number of lags = %ld\n", hdr->num_lags);
  printf("              Number of IFs = %d\n", hdr->nifs);
  printf("   Other information:\n");
  if (hdr->sum==1)
    printf("      IFs are summed.\n");
  if (hdr->freqinversion==1)
    printf("      Frequency band is inverted.\n");
  if (hdr->lagformat==0)
    printf("      Lags are 16 bit integers.\n\n");
  else
    printf("      Lags are 32 bit integers.\n\n");
}


int read_WAPP_rawblock(FILE *infiles[], int numfiles, 
		      unsigned char *data, int *padding)
/* This routine reads a single record from the          */
/* input files *infiles which contain 16 or 32 bit lags */
/* data from the WAPP correlator at Arecibo.            */
/* A WAPP record is ptsperblk_st*numchan_st*#bits long. */
/* *data must be bytesperblk_st bytes long.  If padding */
/* is returned as 1, then padding was added and         */
/* statistics should not be calculated.                 */
{
  int offset=0, numtopad=0, ii;
  unsigned char *dataptr;

  /* If our buffer array is offset from last time */
  /* copy the second part into the first.         */

  if (bufferpts && shiftbuffer){
    offset = bufferpts * numchan_st;
    memcpy(databuffer, databuffer + sampperblk_st, offset);
    dataptr = databuffer + offset;
  } else {
    dataptr = data;
  }
  shiftbuffer=1;

  /* Make sure our current file number is valid */

  if (currentfile >= numfiles)
    return 0;

  /* First, attempt to read data from the current file */
  
  if (fread(lagbuffer, bytesperblk_st, 
	    1, infiles[currentfile])){ /* Got Data */
    /* See if we need to byte-swap and if so, doit */
    if (need_byteswap_st){
      if (bits_per_samp_st==16){
	short *sptr = (short *)lagbuffer;
	for (ii=0; ii<sampperblk_st; ii++, sptr++)
	  *sptr = swap_short(*sptr);
      }
      if (bits_per_samp_st==32){
	int *iptr = (int *)lagbuffer;
	for (ii=0; ii<sampperblk_st; ii++, iptr++)
	  *iptr = swap_int(*iptr);
      }
    }      
    /* Convert from Correlator Lags to Filterbank Powers */
    for (ii=0; ii<ptsperblk_st; ii++)
      convert_WAPP_point(lagbuffer + ii * bytesperpt_st, 
			 dataptr + ii * numchan_st);
    *padding = 0;
    /* Put the new data into the databuffer if needed */
    if (bufferpts){
      memcpy(data, dataptr, sampperblk_st);
    }
    currentblock++;
    return 1;
  } else { /* Didn't get data */
    if (feof(infiles[currentfile])){  /* End of file? */
      numtopad = padpts_st[currentfile] - padnum;
      if (numtopad){  /* Pad the data? */
	*padding = 1;
	if (numtopad >= ptsperblk_st - bufferpts){  /* Lots of padding */
	  if (bufferpts){  /* Buffer the padding? */
	    /* Add the amount of padding we need to */
	    /* make our buffer offset = 0           */
	    numtopad = ptsperblk_st - bufferpts;
	    memset(dataptr, padval, numtopad * numchan_st);
	    /* Copy the new data/padding into the output array */
	    memcpy(data, databuffer, sampperblk_st);
	    bufferpts = 0;
	  } else {  /* Add a full record of padding */
	    numtopad = ptsperblk_st;
	    memset(data, padval, sampperblk_st);
	  }
	  padnum += numtopad;
	  currentblock++;
	  /* If done with padding reset padding variables */
	  if (padnum==padpts_st[currentfile]){
	    padnum = 0;
	    currentfile++;
	  }
	  return 1;
	} else {  /* Need < 1 block (or remaining block) of padding */
	  int pad;
	  /* Add the remainder of the padding and */
	  /* then get a block from the next file. */
          memset(databuffer + bufferpts * numchan_st, 
		 padval, numtopad * numchan_st);
	  padnum = 0;
	  currentfile++;
	  shiftbuffer = 0;
	  bufferpts += numtopad;
	  return read_WAPP_rawblock(infiles, numfiles, data, &pad);
	}
      } else {  /* No padding needed.  Try reading the next file */
	currentfile++;
	shiftbuffer = 0;
	return read_WAPP_rawblock(infiles, numfiles, data, padding);
      }
    } else {
      printf("\nProblem reading record from WAPP data file:\n");
      printf("   currentfile = %d, currentblock = %d.  Exiting.\n",
	     currentfile, currentblock);
      exit(1);
    }
  }
}


int read_WAPP_rawblocks(FILE *infiles[], int numfiles, 
		       unsigned char rawdata[], int numblocks,
		       int *padding)
/* This routine reads numblocks WAPP records from the input  */
/* files *infiles.  The 8-bit filterbank data is returned    */
/* in rawdata which must have a size of numblocks *          */
/* sampperblk_st.  The number  of blocks read is returned.   */
/* If padding is returned as 1, then padding was added       */
/* and statistics should not be calculated                   */
{
  int ii, retval=0, pad, numpad=0;
  
  *padding = 0;
  for (ii=0; ii<numblocks; ii++){
    retval += read_WAPP_rawblock(infiles, numfiles, 
				 rawdata + ii * sampperblk_st, &pad);
    if (pad)
      numpad++;
  }
  /* Return padding 'true' if more than */
  /* half of the blocks are padding.    */
  /* 
     if (numpad > numblocks / 2)
        *padding = 1;
  */
  /* Return padding 'true' if any block was padding */
  if (numpad) 
    *padding = 1;
  return retval;
}


int read_WAPP(FILE *infiles[], int numfiles, float *data, 
	     int numpts, double *dispdelays, int *padding, 
	     int *maskchans, int *nummasked, mask *obsmask)
/* This routine reads numpts from the WAPP raw input   */
/* files *infiles.  These files contain raw correlator */
/* data from WAPP at Arecibo.  Time delays             */
/* and a mask are applied to each channel.  It returns */
/* the # of points read if successful, 0 otherwise.    */
/* If padding is returned as 1, then padding was       */
/* added and statistics should not be calculated.      */
/* maskchans is an array of length numchans contains   */
/* a list of the number of channels that were masked.  */
{
  int ii, jj, numread=0, offset;
  double starttime=0.0;
  static unsigned char *tempzz, *rawdata1, *rawdata2; 
  static unsigned char *currentdata, *lastdata;
  static int firsttime=1, numblocks=1, allocd=0, mask=0;
  static double duration=0.0, timeperblk=0.0;

  *nummasked = 0;
  if (firsttime) {
    if (numpts % ptsperblk_st){
      printf("numpts must be a multiple of %d in read_WAPP()!\n",
	     ptsperblk_st);
      exit(1);
    } else
      numblocks = numpts / ptsperblk_st;
    
    if (obsmask->numchan) mask = 1;
    rawdata1 = gen_bvect(numblocks * sampperblk_st);
    rawdata2 = gen_bvect(numblocks * sampperblk_st);
    allocd = 1;
    timeperblk = ptsperblk_st * dt_st;
    duration = numblocks * timeperblk;
    
    currentdata = rawdata1;
    lastdata = rawdata2;

    numread = read_WAPP_rawblocks(infiles, numfiles, currentdata, 
				  numblocks, padding);
    if (numread != numblocks && allocd){
      printf("Problem reading the raw WAPP data file.\n\n");
      free(rawdata1);
      free(rawdata2);
      rfftw_destroy_plan(fftplan);
      allocd = 0;
      return 0;
    }
    
    if (mask){
      starttime = currentblock * timeperblk;
      *nummasked = check_mask(starttime, duration, obsmask, maskchans);
      if (*nummasked==-1) /* If all channels are masked */
	memset(currentdata, padval, numblocks * sampperblk_st);
    }
    if (*nummasked > 0){ /* Only some of the channels are masked */
      for (ii=0; ii<numpts; ii++){
	offset = ii * numchan_st;
	for (jj=0; jj<*nummasked; jj++)
	  currentdata[offset+maskchans[jj]] = padval;
      }
    }

    SWAP(currentdata, lastdata);
    firsttime=0;
  }
  
  /* Read and de-disperse */
  
  if (allocd){
    numread = read_WAPP_rawblocks(infiles, numfiles, currentdata, 
				  numblocks, padding);
    if (mask){
      starttime = currentblock * timeperblk;
      *nummasked = check_mask(starttime, duration, obsmask, maskchans);
      if (*nummasked==-1) /* If all channels are masked */
	memset(currentdata, padval, numblocks * sampperblk_st);
    }
    if (*nummasked > 0){ /* Only some of the channels are masked */
      for (ii=0; ii<numpts; ii++){
	offset = ii * numchan_st;
	for (jj=0; jj<*nummasked; jj++)
	  currentdata[offset+maskchans[jj]] = padval;
      }
    }

    dedisp(currentdata, lastdata, numpts, numchan_st, dispdelays, data);
    SWAP(currentdata, lastdata);

    if (numread != numblocks){
      free(rawdata1);
      free(rawdata2);
      rfftw_destroy_plan(fftplan);
      allocd = 0;
    }
    return numread * ptsperblk_st;
  } else {
    return 0;
  }
}

void get_WAPP_channel(int channum, float chandat[], 
		     unsigned char rawdata[], int numblocks)
/* Return the values for channel 'channum' of a block of       */
/* 'numblocks' raw WAPP data stored in 'rawdata' in 'chandat'. */
/* 'rawdata' should have been initialized using                */
/* read_WAPP_rawblocks(), and 'chandat' must have at least     */
/* 'numblocks' * 'ptsperblk_st' spaces.                        */
/* Channel 0 is assumed to be the lowest freq channel.         */
{
  int ii, jj;

  if (channum > numchan_st * numifs_st || channum < 0){
    printf("\nchannum = %d is out of range in get_WAPP_channel()!\n\n",
	   channum);
    exit(1);
  }
  /* Select the correct channel */
  for (ii=0, jj=channum; 
       ii<numblocks*ptsperblk_st; 
       ii++, jj+=numchan_st)
    chandat[ii] = rawdata[jj];
}


int read_WAPP_subbands(FILE *infiles[], int numfiles, float *data, 
		       double *dispdelays, int numsubbands, 
		       int transpose, int *padding, 
		       int *maskchans, int *nummasked, mask *obsmask)
/* This routine reads a record from the input files *infiles[]   */
/* which contain data from the WAPP system.  The routine uses    */
/* dispersion delays in 'dispdelays' to de-disperse the data     */
/* into 'numsubbands' subbands.  It stores the resulting data    */
/* in vector 'data' of length 'numsubbands' * 'ptsperblk_st'.    */
/* The low freq subband is stored first, then the next highest   */
/* subband etc, with 'ptsperblk_st' floating points per subband. */
/* It returns the # of points read if succesful, 0 otherwise.    */
/* If padding is returned as 1, then padding was added and       */
/* statistics should not be calculated.  'maskchans' is an array */
/* of length numchans which contains a list of the number of     */
/* channels that were masked.  The # of channels masked is       */
/* returned in 'nummasked'.  'obsmask' is the mask structure     */
/* to use for masking.  If 'transpose'==0, the data will be kept */
/* in time order instead of arranged by subband as above.        */
{
  int ii, jj, numread, trtn, offset;
  double starttime=0.0;
  static unsigned char *tempzz;
  static unsigned char rawdata1[WAPP_MAXDATLEN], rawdata2[WAPP_MAXDATLEN]; 
  static unsigned char *currentdata, *lastdata, *move;
  static int firsttime=1, move_size=0, mask=0;
  static double timeperblk=0.0;
  
  *nummasked = 0;
  if (firsttime) {
    if (obsmask->numchan) mask = 1;
    move_size = (ptsperblk_st + numsubbands) / 2;
    move = gen_bvect(move_size);
    currentdata = rawdata1;
    lastdata = rawdata2;
    timeperblk = ptsperblk_st * dt_st;
    if (!read_WAPP_rawblock(infiles, numfiles, currentdata, padding)){
      printf("Problem reading the raw WAPP data file.\n\n");
      return 0;
    }
    if (mask){
      starttime = currentblock * timeperblk;
      *nummasked = check_mask(starttime, timeperblk, obsmask, maskchans);
      if (*nummasked==-1) /* If all channels are masked */
	memset(currentdata, padval, ptsperblk_st);
    }
    if (*nummasked > 0){ /* Only some of the channels are masked */
      for (ii=0; ii<ptsperblk_st; ii++){
	offset = ii * numchan_st;
	for (jj=0; jj<*nummasked; jj++)
	  currentdata[offset+maskchans[jj]] = padval;
      }
    }
    SWAP(currentdata, lastdata);
    firsttime=0;
  }

  /* Read and de-disperse */

  numread = read_WAPP_rawblock(infiles, numfiles, currentdata, padding);
  if (mask){
    starttime = currentblock * timeperblk;
    *nummasked = check_mask(starttime, timeperblk, obsmask, maskchans);
    if (*nummasked==-1) /* If all channels are masked */
      memset(currentdata, padval, ptsperblk_st);
  }
  if (*nummasked > 0){ /* Only some of the channels are masked */
    for (ii=0; ii<ptsperblk_st; ii++){
      offset = ii * numchan_st;
      for (jj=0; jj<*nummasked; jj++)
	currentdata[offset+maskchans[jj]] = padval;
    }
  }
  dedisp_subbands(currentdata, lastdata, ptsperblk_st, numchan_st, 
		  dispdelays, numsubbands, data);
  SWAP(currentdata, lastdata);

  /* Transpose the data into vectors in the result array */

  if (transpose){
    if ((trtn = transpose_float(data, ptsperblk_st, numsubbands,
				move, move_size))<0)
      printf("Error %d in transpose_float().\n", trtn);
  }
  if (numread)
    return ptsperblk_st;
  else
    return 0;
}


void convert_WAPP_point(void *rawdata, unsigned char *bytes)
/* This routine converts a single point of WAPP lags   */
/* into a filterbank style array of bytes.             */
/* Van Vleck corrections are applied but no window     */
/* functions can be applied as of yet...               */
{
  int ii, two_nlags;
  double power, pfact;
  static float acf[2*WAPP_MAXLAGS], lag[WAPP_MAXLAGS];

  /* Fill lag array with scaled CFs */
  if (bits_per_samp_st==16){
    unsigned short *sdata=(unsigned short *)rawdata;
    for (ii=0; ii<numchan_st; ii++)
      lag[ii] = corr_scale_st * sdata[ii] - 1.0;
  } else {
    unsigned int *idata=(unsigned int *)rawdata;
    for (ii=0; ii<numchan_st; ii++)
      lag[ii] = corr_scale_st * idata[ii] - 1.0;
  }

  /* Calculate power */
  power = inv_cerf(lag[0]);
  power = 0.1872721836 / (power * power);
  
  /* Apply Van Vleck Corrections to the Lags */
  if (corr_level_st==3)
    vanvleck3lev(lag, numchan_st);
  else if (corr_level_st==9)
    vanvleck9lev(lag, numchan_st);
  else
    printf("\nError:  corr_level_st (%d) does not equal 3 or 9!\n\n", 
	   corr_level_st);
  
  /* Form even ACF in array */
  two_nlags = 2 * numchan_st;
  for(ii=1; ii<numchan_st; ii++)
    acf[ii] = acf[two_nlags-ii] = power * lag[ii];
  acf[0] = power * lag[0];
  acf[numchan_st] = 0.0;
 
  /* FFT the ACF (which is real and even) -> real and even FFT */
  rfftw_one(fftplan, acf, lag);
  
  /* Reverse band if it needs it */
  if (decreasing_freqs_st){
    float tempzz=0.0, *loptr, *hiptr;
    loptr = lag + 0;
    hiptr = lag + numchan_st - 1;
    for (ii=0; ii<numchan_st/2; ii++, loptr++, hiptr--)
      SWAP(*loptr, *hiptr);
  }

  /* Scale and pack the powers */
  pfact = 255.0 / (scale_max_st - scale_min_st);
  for(ii=0; ii<numchan_st; ii++)
    bytes[ii] = (unsigned char) ((lag[ii] - scale_min_st) * pfact + 0.5);
#if 0
  { /* Show what the raw powers are (avg ~1.05, var ~0.2) */
    double avg, var;
    avg_var(lag, numchan_st, &avg, &var);
    printf("avg = %f    var = %f\n", avg, var);
  }
#endif
}


static double inv_cerf(double input)
/* Approximation for Inverse Complementary Error Function */
{
  static double numerator_const[3] = {
    1.591863138, -2.442326820, 0.37153461};
  static double denominator_const[3] = {
    1.467751692, -3.013136362, 1.0};
  double num, denom, temp_data, temp_data_srq, erf_data;

  erf_data = 1.0 - input;
  temp_data = erf_data * erf_data - 0.5625;
  temp_data_srq = temp_data * temp_data;
  num = erf_data * (numerator_const[0] + 
		    (temp_data * numerator_const[1]) + 
		    (temp_data_srq * numerator_const[2]));
  denom = denominator_const[0] + temp_data * denominator_const[1] + 
    temp_data_srq * denominator_const[2];
  return num/denom;
}


#define NO    0
#define YES   1
/*------------------------------------------------------------------------*
 * Van Vleck Correction for 9-level sampling/correlation
 *  Samples {-4,-3,-2,-1,0,1,2,3,4}
 * Uses Zerolag to adjust correction
 *   data_array -> Points into ACF of at least 'count' points
 * This routine takes the first value as the zerolag and corrects the
 * remaining count-1 points.  Zerolag is set to a normalized 1
 * NOTE - The available routine works on lags normaized to -16<rho<16, so
 *  I need to adjust the values before/after the fit
 * Coefficent ranges
 *   c1
 *     all
 *   c2
 *     r0 > 4.5
 *     r0 < 2.1
 *     rest
 * NOTE - correction is done INPLACE ! Original values are destroyed
 * As reported by M. Lewis -> polynomial fits are OK, but could be improved
 *------------------------------------------------------------------------*/
static void vanvleck9lev(float *rho, int npts)
{
  double acoef[5], dtmp, zl;
  int i;
  static double coef1[5] =
    {1.105842267, -0.053258115, 0.011830276, -0.000916417, 0.000033479};
  static double coef2rg4p5[5] =
    {0.111705575, -0.066425925, 0.014844439, -0.001369796, 0.000044119};
  static double coef2rl2p1[5] =
    {1.285303775, -1.472216011, 0.640885537, -0.123486209, 0.008817175};
  static double coef2rother[5] =
    {0.519701391, -0.451046837, 0.149153116, -0.021957940, 0.001212970};
  static double coef3rg2p0[5] =
    {1.244495105, -0.274900651, 0.022660239, -0.000760938, -1.993790548};
  static double coef3rother[5] =
    {1.249032787, 0.101951346, -0.126743165, 0.015221707, -2.625961708};
  static double coef4rg3p15[5] =
    {0.664003237, -0.403651682, 0.093057131, -0.008831547, 0.000291295};
  static double coef4rother[5] =
    {9.866677289, -12.858153787, 6.556692205, -1.519871179, 0.133591758};
  static double coef5rg4p0[4] =
    {0.033076469, -0.020621902, 0.001428681, 0.000033733};
  static double coef5rg2p2[4] =
    {5.284269565, 6.571535249, -2.897741312, 0.443156543};
  static double coef5rother[4] =
    {-1.475903733, 1.158114934, -0.311659264, 0.028185170};
  
  zl = rho[0] * 16;
  /* ro = *rho;                 */
  /*  for(i=0; i<npts; i++)     */
  /*    (rho+i) *= *(rho+i)/ro; */
  acoef[0] =
    ((((coef1[4] * zl + coef1[3]) * zl + coef1[2]) * zl +
      coef1[1]) * zl + coef1[0]);
  if (zl > 4.50)
    acoef[1] =
      ((((coef2rg4p5[4] * zl + coef2rg4p5[3]) * zl +
	 coef2rg4p5[2]) * zl + coef2rg4p5[1]) * zl + coef2rg4p5[0]);
  else if (zl < 2.10)
    acoef[1] =
      ((((coef2rl2p1[4] * zl + coef2rl2p1[3]) * zl +
	 coef2rl2p1[2]) * zl + coef2rl2p1[1]) * zl + coef2rl2p1[0]);
  else
    acoef[1] =
      ((((coef2rother[4] * zl + coef2rother[3]) * zl +
	 coef2rother[2]) * zl + coef2rother[1]) * zl +
       coef2rother[0]);
  if (zl > 2.00)
    acoef[2] =
      coef3rg2p0[4] / zl +
      (((coef3rg2p0[3] * zl + coef3rg2p0[2]) * zl +
	coef3rg2p0[1]) * zl + coef3rg2p0[0]);
  else
    acoef[2] =
      coef3rother[4] / zl +
      (((coef3rother[3] * zl + coef3rother[2]) * zl +
	coef3rother[1]) * zl + coef3rother[0]);
  if (zl > 3.15)
    acoef[3] =
      ((((coef4rg3p15[4] * zl + coef4rg3p15[3]) * zl +
	 coef4rg3p15[2]) * zl + coef4rg3p15[1]) * zl +
       coef4rg3p15[0]);
  else
    acoef[3] =
      ((((coef4rg3p15[4] * zl + coef4rother[3]) * zl +
	 coef4rother[2]) * zl + coef4rother[1]) * zl +
       coef4rother[0]);
  if (zl > 4.00)
    acoef[4] =
      (((coef5rg4p0[3] * zl + coef5rg4p0[2]) * zl +
	coef5rg4p0[1]) * zl + coef5rg4p0[0]);
  else if (zl < 2.2)
    acoef[4] =
      (((coef5rg2p2[3] * zl + coef5rg2p2[2]) * zl +
	coef5rg2p2[1]) * zl + coef5rg2p2[0]);
  else
    acoef[4] =
      (((coef5rother[3] * zl + coef5rother[2]) * zl +
	coef5rother[1]) * zl + coef5rother[0]);
  for (i = 1; i < npts; i++) {
    dtmp = rho[i];
    rho[i] =
      ((((acoef[4] * dtmp + acoef[3]) * dtmp + acoef[2]) * dtmp +
	acoef[1]) * dtmp + acoef[0]) * dtmp;
  }
  rho[0] = 1.0;
  return;
}

/*------------------------------------------------------------------------*
 * Van Vleck Correction for 3-level sampling/correlation
 *  Samples {-1,0,1}
 * Uses Zerolag to adjust correction
 *   data_array -> Points into ACF of at least 'count' points
 * This routine takes the first value as the zerolag and corrects the
 * remaining count-1 points.  Zerolag is set to a normalized 1
 *
 * NOTE - correction is done INPLACE ! Original values are destroyed
 *------------------------------------------------------------------------*/
static void vanvleck3lev(float *rho, int npts)
{
  double lo_u[3], lo_h[3];
  double high_u[5], high_h[5];
  double lo_coefficient[3];
  double high_coefficient[5];
  double zho, zho_3;
  double temp_data;
  double temp_data_1;
  int ichan, ico, flag_any_high;
  static double lo_const[3][4] = {
    {0.939134371719, -0.567722496249, 1.02542540932, 0.130740914912},
    {-0.369374472755, -0.430065136734, -0.06309459132, -0.00253019992917},
    {0.888607422108, -0.230608118885, 0.0586846424223, 0.002012775510695}
  };
  static double high_const[5][4] = {
    {-1.83332160595, 0.719551585882, 1.214003774444, 7.15276068378e-5},
    {1.28629698818, -1.45854382672, -0.239102591283, -0.00555197725185},
    {-7.93388279993, 1.91497870485, 0.351469403030, 0.00224706453982},
    {8.04241371651, -1.51590759772, -0.18532022393, -0.00342644824947},
    {-13.076435520, 0.769752851477, 0.396594438775, 0.0164354218208}
  };
  
  /* Perform Lo correction on All data that is not flaged 
     for high correction  */
  zho = (double) rho[0];
  zho_3 = zho * zho * zho;
  lo_u[0] = zho;
  lo_u[1] = zho_3 - (61.0 / 512.0);
  lo_u[2] = zho - (63.0 / 128.0);
  lo_h[0] = zho * zho;
  lo_h[2] = zho_3 * zho_3 * zho;      /* zlag ^7 */
  lo_h[1] = zho * lo_h[2];    /* zlag ^8 */
  /* determine lo-correct coefficents -*/
  for (ico = 0; ico < 3; ico++) {
    lo_coefficient[ico] =
      (lo_u[ico] *
       (lo_u[ico] *
	(lo_u[ico] * lo_const[ico][0] + lo_const[ico][1]) +
	lo_const[ico][2]) + lo_const[ico][3]) / lo_h[ico];
  }
  /* perform correction --*/
  for (ichan = 1, flag_any_high = NO; ichan < npts; ichan++) {
    temp_data = (double) rho[ichan];
    if (fabs(temp_data) > 0.199) {
      if (flag_any_high == NO) {
	high_u[0] = lo_h[2];    /* zlag ^7 */
	high_u[1] = zho - (63.0 / 128.0);
	high_u[2] = zho * zho - (31.0 / 128.0);
	high_u[3] = zho_3 - (61.0 / 512.0);
	high_u[4] = zho - (63.0 / 128.0);
	high_h[0] = lo_h[1];    /* zlag ^8 */
	high_h[1] = lo_h[1];    /* zlag ^8 */
	high_h[2] = lo_h[1] * zho_3 * zho;      /* zlag ^12 */
	high_h[3] = lo_h[1] * lo_h[1] * zho;    /* zlag ^17 */
	high_h[4] = high_h[3];  /* zlag ^17 */
	for (ico = 0; ico < 5; ico++) {
	  high_coefficient[ico] =
	    (high_u[ico] *
	     (high_u[ico] *
	      (high_u[ico] * high_const[ico][0] +
	       high_const[ico][1]) + high_const[ico][2]) +
	     high_const[ico][3]) / high_h[ico];
	}
	flag_any_high = YES;
      }
      temp_data_1 = fabs(temp_data * temp_data * temp_data);
      rho[ichan] =
	(temp_data *
	 (temp_data_1 *
	  (temp_data_1 *
	   (temp_data_1 *
	    (temp_data_1 * high_coefficient[4] +
	     high_coefficient[3]) + high_coefficient[2]) +
	   high_coefficient[1]) + high_coefficient[0]));
    } else {
      temp_data_1 = temp_data * temp_data;
      rho[ichan] =
	(temp_data *
	 (temp_data_1 *
	  (temp_data_1 * lo_coefficient[2] + lo_coefficient[1]) +
	  lo_coefficient[0]));
    }
  }
  rho[0] = 1.0;
}
