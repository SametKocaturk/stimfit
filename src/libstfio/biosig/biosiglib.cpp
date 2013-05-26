// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.

// You should have received a copy of the GNU General Public License
// along with this program; if not, write to the Free Software
// Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.

// Copyright 2012,2013 Alois Schloegl, IST Austria <alois.schloegl@ist.ac.at>

#include <string>
#include <iomanip>
#include <vector>
#include <boost/shared_ptr.hpp>
#include <boost/shared_array.hpp>
#include <sstream>
#include <string.h>

#include "../stfio.h"

#if 1
#if defined(__GNUC__)
#include <biosig.h>
#elif defined(_MSC_VER)
/* level 2 interface of libbiosig is required for ABI compatibility */
#include <biosig2.h>
#endif

/* these are internal biosig functions, defined in biosig-dev.h which is not always available */
extern "C" size_t ifwrite(void* buf, size_t size, size_t nmemb, HDRTYPE* hdr);
extern "C" uint32_t lcm(uint32_t A, uint32_t B);
#if !defined(__MINGW32__) && !defined(_MSC_VER)
#include <endian.h>	// not available on mingw
#endif
#endif

#include "./biosiglib.h"

/* Redefine BIOSIG_VERSION for versions < 1 */
#if (BIOSIG_VERSION_MAJOR < 1)
#undef BIOSIG_VERSION
#define BIOSIG_VERSION (BIOSIG_VERSION_MAJOR * 10000 + BIOSIG_VERSION_MINOR * 100 + BIOSIG_PATCHLEVEL)
#endif

void stfio::importBSFile(const std::string &fName, Recording &ReturnData, ProgressInfo& progDlg) {

    std::string errorMsg("Exception while calling std::importBSFile():\n");
    std::string yunits;
    // =====================================================================================================================
    //
    // Open file with libbiosig and read in the data
    //
    // There basically two implementations, one with libbiosig before v1.6.0 and
    // and one for libbiosig v1.6.0 and later
    //
    // =====================================================================================================================


#ifdef __LIBBIOSIG2_H__

    HDRTYPE* hdr =  sopen( fName.c_str(), "r", NULL );
    if (hdr==NULL) {
        errorMsg += "\nBiosig header is empty";
        ReturnData.resize(0);
        throw std::runtime_error(errorMsg.c_str());
    }
    if (biosig_check_filetype(hdr, ABF) && biosig_check_error(hdr)) {
        /* this triggers the fall back mechanims w/o reporting an error message */
        ReturnData.resize(0);
        destructHDR(hdr);	// free allocated memory
        throw std::runtime_error(errorMsg.c_str());
    }
    errorMsg += "\n";
    if (serror2(hdr)) {
        errorMsg += std::string(biosig_get_errormsg(hdr));
        ReturnData.resize(0);
        destructHDR(hdr);	// free allocated memory
        throw std::runtime_error(errorMsg.c_str());
    }

    // ensure the event table is in chronological order	
    sort_eventtable(hdr);

    // allocate local memory for intermediate results;
    const int strSize=100;
    char str[strSize];

    /*
	count sections and generate list of indices indicating start and end of sweeps
     */	

    double fs = biosig_get_eventtable_samplerate(hdr);
    size_t numberOfEvents = biosig_get_number_of_events(hdr);
    uint32_t nsections = biosig_get_number_of_segments(hdr);
    size_t *SegIndexList = (size_t*)malloc((nsections+1)*sizeof(size_t));
    SegIndexList[0] = 0;
    SegIndexList[nsections] = biosig_get_number_of_samples(hdr);
    std::string annotationTableDesc = std::string();
    for (size_t k=0, n=0; k < numberOfEvents; k++) {
        uint32_t pos;
        uint16_t typ;
        const char *desc;
        /*
        uint32_t dur;
        uint16_t chn;
        gdftype  timestamp;
        */
        biosig_get_nth_event(hdr, k, &typ, &pos, NULL, NULL, NULL, &desc);

        if (typ == 0x7ffe) {
            SegIndexList[++n] = pos;
        }
        else if (typ < 256) {
            sprintf(str,"%f s:\t",pos/fs);
            annotationTableDesc += std::string( str ) + std::string( desc ) + "\n" ;
        }
    }
    int numberOfChannels = biosig_get_number_of_channels(hdr);

    /*************************************************************************
        rescale data to mV and pA
     *************************************************************************/
    for (int ch=0; ch < numberOfChannels; ++ch) {
        CHANNEL_TYPE *hc = biosig_get_channel(hdr, ch);
        switch (hc->PhysDimCode & 0xffe0) {
        case 4256:  // Volt
		//biosig_channel_scale_to_unit(hc, "mV");
		biosig_channel_change_scale_to_physdimcode(hc, 4272);
		break;
        case 4160:  // Ampere
		//biosig_channel_scale_to_unit(hc, "pA");
		biosig_channel_change_scale_to_physdimcode(hc, 4181);
		break;
	    }
    }

    /*************************************************************************
        read bulk data
     *************************************************************************/
    biosig_data_type *data = biosig_get_data(hdr, 0);
    size_t SPR = biosig_get_number_of_samples(hdr);

#ifdef _STFDEBUG
    std::cout << "Number of events: " << numberOfEvents << std::endl;
    /*int res = */ hdr2ascii(hdr, stdout, 4);
#endif

    for (int NS=0; NS < numberOfChannels; ) {
        CHANNEL_TYPE *hc = biosig_get_channel(hdr, NS);

        Channel TempChannel(nsections);
        TempChannel.SetChannelName(biosig_channel_get_label(hc));
        TempChannel.SetYUnits(biosig_channel_get_physdim(hc));

        for (size_t ns=1; ns<=nsections; ns++) {
	        size_t SPS = SegIndexList[ns]-SegIndexList[ns-1];	// length of segment, samples per segment

		int progbar = 100.0*(1.0*ns/nsections + NS)/numberOfChannels;
		std::ostringstream progStr;
		progStr << "Reading channel #" << NS + 1 << " of " << numberOfChannels
			<< ", Section #" << ns << " of " << nsections;
		progDlg.Update(progbar, progStr.str());

		/* unused //
		char sweepname[20];
		sprintf(sweepname,"sweep %i",(int)ns);
		*/
		Section TempSection(
                                SPS, // TODO: hdr->nsamplingpoints[nc][ns]
                                "" // TODO: hdr->sectionname[nc][ns]
        );

		std::copy(&(data[NS*SPR + SegIndexList[ns-1]]),
			  &(data[NS*SPR + SegIndexList[ns]]),
			  TempSection.get_w().begin() );

        try {
            TempChannel.InsertSection(TempSection, ns-1);
        }
        catch (...) {
			ReturnData.resize(0);
			destructHDR(hdr);
			throw;
		}
	}
    try {
        if ((int)ReturnData.size() < numberOfChannels) {
            ReturnData.resize(numberOfChannels);
		}
		ReturnData.InsertChannel(TempChannel, NS++);
    }
    catch (...) {
		ReturnData.resize(0);
		destructHDR(hdr);
		throw;
        }
    }

    free(SegIndexList);

    ReturnData.SetComment ( biosig_get_recording_id(hdr) );

    sprintf(str,"v%i.%i.%i (compiled on %s %s)",BIOSIG_VERSION_MAJOR,BIOSIG_VERSION_MINOR,BIOSIG_PATCHLEVEL,__DATE__,__TIME__);
    std::string Desc = std::string("importBiosig with libbiosig ")+std::string(str) + " ";

    const char* tmpstr;
    if ((tmpstr=biosig_get_technician(hdr)))
            Desc += std::string ("\nTechnician:\t") + std::string (tmpstr) + " ";
    Desc += std::string( "\nCreated with: ");
    if ((tmpstr=biosig_get_manufacturer_name(hdr)))
        Desc += std::string( tmpstr ) + " ";
    if ((tmpstr=biosig_get_manufacturer_model(hdr)))
        Desc += std::string( tmpstr ) + " ";
    if ((tmpstr=biosig_get_manufacturer_version(hdr)))
        Desc += std::string( tmpstr ) + " ";
    if ((tmpstr=biosig_get_manufacturer_serial_number(hdr)))
        Desc += std::string( tmpstr ) + " ";

    Desc += std::string ("\nUser specified Annotations:\n")+annotationTableDesc;

    ReturnData.SetFileDescription(Desc);
    //ReturnData.SetGlobalSectionDescription(Desc);

    ReturnData.SetXScale(1000.0/biosig_get_samplerate(hdr));
    ReturnData.SetXUnits("ms");
    ReturnData.SetScaling("biosig scaling factor");

    /*************************************************************************
        Date and time conversion
     *************************************************************************/
    struct tm T;
    biosig_get_startdatetime(hdr, &T);

    strftime(str,strSize,"%Y-%m-%d",&T);	// %F
    ReturnData.SetDate(str);
    strftime(str,strSize,"%H:%M:%S",&T);	// %D
    ReturnData.SetTime(str);

#ifdef MODULE_ONLY
    if (progress) {
        std::cout << "\r";
        std::cout << "100%" << std::endl;
    }
#endif
    destructHDR(hdr);


#else  // #ifndef __LIBBIOSIG2_H__


    HDRTYPE* hdr =  sopen( fName.c_str(), "r", NULL );
    if (hdr==NULL) {
        errorMsg += "\nBiosig header is empty";
        ReturnData.resize(0);
        throw std::runtime_error(errorMsg.c_str());
    }
#if !defined(BIOSIG_VERSION) || (BIOSIG_VERSION < 10501)
    if (hdr->TYPE==ABF) {
        /*
           biosig v1.5.0 and earlier does not always return
           with a proper error message for ABF files.
           This causes problems with the ABF fallback mechanism
        */
#else
    if (hdr->TYPE==ABF && hdr->AS.B4C_ERRNUM) {
        /* this triggers the fall back mechanims w/o reporting an error message */
#endif
        ReturnData.resize(0);
        destructHDR(hdr);	// free allocated memory
        throw std::runtime_error(errorMsg.c_str());
    }
    errorMsg += "\n";
#if defined(BIOSIG_VERSION) && (BIOSIG_VERSION > 10400)
    if (serror2(hdr)) {
        errorMsg += std::string(hdr->AS.B4C_ERRMSG);
#else
    if (serror()) {
	errorMsg += std::string(B4C_ERRMSG);
#endif
        ReturnData.resize(0);
        destructHDR(hdr);	// free allocated memory
        throw std::runtime_error(errorMsg.c_str());
    }

    // ensure the event table is in chronological order
    sort_eventtable(hdr);

    // allocate local memory for intermediate results;
    const int strSize=100;
    char str[strSize];

    /*
	count sections and generate list of indices indicating start and end of sweeps
     */
    size_t numberOfEvents = hdr->EVENT.N;
    size_t LenIndexList = 256;
    if (LenIndexList > numberOfEvents) LenIndexList = numberOfEvents + 2;
    size_t *SegIndexList = (size_t*)malloc(LenIndexList*sizeof(size_t));
    uint32_t nsections = 0;
    SegIndexList[nsections] = 0;
    size_t MaxSectionLength = 0;
    for (size_t k=0; k <= numberOfEvents; k++) {
        if (LenIndexList <= nsections+2) {
            // allocate more memory as needed
		    LenIndexList *=2;
		    SegIndexList = (size_t*)realloc(SegIndexList, LenIndexList*sizeof(size_t));
	    }
        /*
            count number of sections and stores it in nsections;
            EVENT.TYP==0x7ffe indicate number of breaks between sweeps
	        SegIndexList includes index to first sample and index to last sample,
	        thus, the effective length of SegIndexList is the number of 0x7ffe plus two.
	    */
        if (0)                              ;
        else if (k >= hdr->EVENT.N)         SegIndexList[++nsections] = hdr->NRec*hdr->SPR;
        else if (hdr->EVENT.TYP[k]==0x7ffe) SegIndexList[++nsections] = hdr->EVENT.POS[k];
        else                                continue;

        size_t SPS = SegIndexList[nsections]-SegIndexList[nsections-1];	// length of segment, samples per segment
	    if (MaxSectionLength < SPS) MaxSectionLength = SPS;
    }

    int numberOfChannels = 0;
    for (int k=0; k < hdr->NS; k++)
        if (hdr->CHANNEL[k].OnOff==1)
            numberOfChannels++;

    /*************************************************************************
        rescale data to mV and pA
     *************************************************************************/    
    for (int ch=0; ch < hdr->NS; ++ch) {
        CHANNEL_TYPE *hc = hdr->CHANNEL+ch;
        if (hc->OnOff != 1) continue;
        double scale = PhysDimScale(hc->PhysDimCode); 
        switch (hc->PhysDimCode & 0xffe0) {
        case 4256:  // Volt
                hc->PhysDimCode = 4274; // = PhysDimCode("mV");
                scale *=1e3;   // V->mV
                hc->PhysMax *= scale;         
                hc->PhysMin *= scale;         
                hc->Cal *= scale;         
                hc->Off *= scale;         
                break; 
        case 4160:  // Ampere
                hc->PhysDimCode = 4181; // = PhysDimCode("pA");
                scale *=1e12;   // A->pA
                hc->PhysMax *= scale;         
                hc->PhysMin *= scale;         
                hc->Cal *= scale;         
                hc->Off *= scale;         
                break; 
        }     
    }

    /*************************************************************************
        read bulk data 
     *************************************************************************/    
    hdr->FLAG.ROW_BASED_CHANNELS = 0;
    /* size_t blks = */ sread(NULL, 0, hdr->NRec, hdr);
    biosig_data_type *data = hdr->data.block;
    size_t SPR = hdr->NRec*hdr->SPR;

#ifdef _STFDEBUG
    std::cout << "Number of events: " << numberOfEvents << std::endl;
    /*int res = */ hdr2ascii(hdr, stdout, 4);
#endif

    int NS = 0;   // number of non-empty channels
    for (size_t nc=0; nc < hdr->NS; ++nc) {

        if (hdr->CHANNEL[nc].OnOff == 0) continue;

        Channel TempChannel(nsections);
        TempChannel.SetChannelName(hdr->CHANNEL[nc].Label);
#if defined(BIOSIG_VERSION) && (BIOSIG_VERSION > 10301)
        TempChannel.SetYUnits(PhysDim3(hdr->CHANNEL[nc].PhysDimCode));
#else
        PhysDim(hdr->CHANNEL[nc].PhysDimCode,str);
        TempChannel.SetYUnits(str);
#endif

        for (size_t ns=1; ns<=nsections; ns++) {
	        size_t SPS = SegIndexList[ns]-SegIndexList[ns-1];	// length of segment, samples per segment

		int progbar = 100.0*(1.0*ns/nsections + NS)/numberOfChannels;
		std::ostringstream progStr;
		progStr << "Reading channel #" << NS + 1 << " of " << numberOfChannels
			<< ", Section #" << ns << " of " << nsections;
		progDlg.Update(progbar, progStr.str());

		/* unused //
		char sweepname[20];
		sprintf(sweepname,"sweep %i",(int)ns);		
		*/
		Section TempSection(
                                SPS, // TODO: hdr->nsamplingpoints[nc][ns]
                                "" // TODO: hdr->sectionname[nc][ns]
            	);

		std::copy(&(data[NS*SPR + SegIndexList[ns-1]]),
			  &(data[NS*SPR + SegIndexList[ns]]),
			  TempSection.get_w().begin() );

        try {
            TempChannel.InsertSection(TempSection, ns-1);
        }
        catch (...) {
			ReturnData.resize(0);
			destructHDR(hdr);
			throw;
		}
	}        
    try {
        if ((int)ReturnData.size() < numberOfChannels) {
            ReturnData.resize(numberOfChannels);
		}
		ReturnData.InsertChannel(TempChannel, NS++);
    }
    catch (...) {
		ReturnData.resize(0);
		destructHDR(hdr);
		throw;
        }
    }

    free(SegIndexList); 	

    ReturnData.SetComment ( hdr->ID.Recording );

    sprintf(str,"v%i.%i.%i (compiled on %s %s)",BIOSIG_VERSION_MAJOR,BIOSIG_VERSION_MINOR,BIOSIG_PATCHLEVEL,__DATE__,__TIME__);
    std::string Desc = std::string("importBiosig with libbiosig ")+std::string(str);

    if (hdr->ID.Technician)
            Desc += std::string ("\nTechnician:\t") + std::string (hdr->ID.Technician);
    Desc += std::string( "\nCreated with: ");
    if (hdr->ID.Manufacturer.Name)
        Desc += std::string( hdr->ID.Manufacturer.Name );
    if (hdr->ID.Manufacturer.Model)
        Desc += std::string( hdr->ID.Manufacturer.Model );
    if (hdr->ID.Manufacturer.Version)
        Desc += std::string( hdr->ID.Manufacturer.Version );
    if (hdr->ID.Manufacturer.SerialNumber)
        Desc += std::string( hdr->ID.Manufacturer.SerialNumber );

    Desc += std::string ("\nUser specified Annotations:\n");
    for (size_t k=0; k < numberOfEvents; k++) {
        if (hdr->EVENT.TYP[k] < 256) {
            sprintf(str,"%f s:\t",hdr->EVENT.POS[k]/hdr->EVENT.SampleRate);
            Desc += std::string( str );
            Desc += std::string( hdr->EVENT.CodeDesc[hdr->EVENT.TYP[k]] ) + "\n";
        }
    }
    ReturnData.SetFileDescription(Desc);
    //ReturnData.SetGlobalSectionDescription(Desc);

    ReturnData.SetXScale(1000.0/hdr->SampleRate);
    ReturnData.SetXUnits("ms");
    ReturnData.SetScaling("biosig scaling factor");

    /*************************************************************************
        Date and time conversion
     *************************************************************************/
    struct tm T;
#if (BIOSIG_VERSION_MAJOR > 0)
    gdf_time2tm_time_r(hdr->T0, &T);
#else
    struct tm* Tp;
    Tp = gdf_time2tm_time(hdr->T0);
    T = *Tp;
#endif
    strftime(str,strSize,"%Y-%m-%d",&T);	// %F
    ReturnData.SetDate(str);
    strftime(str,strSize,"%H:%M:%S",&T);	// %D
    ReturnData.SetTime(str);

#ifdef MODULE_ONLY
    if (progress) {
        std::cout << "\r";
        std::cout << "100%" << std::endl;
    }
#endif

    destructHDR(hdr);

#endif

}


    // =====================================================================================================================
    //
    // Save file with libbiosig into GDF format
    //
    // There basically two implementations, one with libbiosig before v1.6.0 and
    // and one for libbiosig v1.6.0 and later
    //
    // =====================================================================================================================

bool stfio::exportBiosigFile(const std::string& fName, const Recording& Data, stfio::ProgressInfo& progDlg) {
/*
    converts the internal data structure to libbiosig's internal structure
    and saves the file as gdf file.

    The data in converted into the raw data format, and not into the common
    data matrix.
*/

#ifdef __LIBBIOSIG2_H__

    int numberOfChannels = Data.size();
    HDRTYPE* hdr = constructHDR(numberOfChannels, 0);

	/* Initialize all header parameters */
    biosig_set_filetype(hdr, GDF);

    struct tm t;
    char *str;
    str = strdup(Data.GetDate().c_str());
    t.tm_year = strtol(str,&str,10)-1900;
    t.tm_mon  = strtol(str+1,&str,10)-1;
    t.tm_mday = strtol(str+1,&str,10);

    str = strdup(Data.GetTime().c_str());
    t.tm_hour = strtol(str,&str,10);
    t.tm_min = strtol(str+1,&str,10);
    t.tm_sec = strtol(str+1,&str,10);

    biosig_get_startdatetime(hdr, &t);

    const char *xunits = Data.GetXUnits().c_str();
    uint16_t pdc = PhysDimCode(xunits);

    if ((pdc & 0xffe0) == PhysDimCode("s")) {
        fprintf(stderr,"Stimfit exportBiosigFile: xunits [%s] has not proper units, assume [ms]\n",Data.GetXUnits().c_str());
        pdc = PhysDimCode("ms");
    }

    double fs = 1.0/(PhysDimScale(pdc) * Data.GetXScale());
    biosig_set_samplerate(hdr, fs);

    biosig_set_number_of_samples_per_record(hdr,1);

    biosig_set_flags(hdr, 0, 0, 0);

    /* Initialize all channel parameters */
    size_t k, m, numberOfEvents=0;
    size_t NRec=0;
    for (k = 0; k < numberOfChannels; ++k) {
        CHANNEL_TYPE *hc = biosig_get_channel(hdr, k);

	biosig_channel_set_datatype_to_double(hc);
	biosig_channel_set_scaling(hc, -1e9, 1e9, -1e9, 1e9);
	biosig_channel_set_label(hc, Data[k].GetChannelName().c_str());
	biosig_channel_set_physdim(hc, Data[k].GetYUnits().c_str());

        /* Channel descriptions. */
        hc->PhysDimCode = PhysDimCode(Data[k].GetYUnits().c_str());

	biosig_channel_set_filter(hc, NAN, NAN, NAN);
	biosig_channel_set_timing_offset(hc, 0.0);
	biosig_channel_set_impedance(hc, NAN);

        // TODO replace accessing fields of struct
        hc->SPR    = hdr->SPR;

        // each segment gets one marker, roughly
        numberOfEvents += Data[k].size();

        size_t m,len = 0;
        for (len=0, m = 0; m < Data[k].size(); ++m) {
            unsigned div = lround(Data[k][m].GetXScale()/Data.GetXScale());
            hc->SPR = lcm(hc->SPR,div);  // sampling interval of m-th segment in k-th channel
            len += div*Data[k][m].size();
        }
        hdr->SPR = lcm(hdr->SPR, hc->SPR);

        if (k==0) {
            NRec = len;
        }
        else if ((size_t)NRec != len) {
            destructHDR(hdr);
            throw std::runtime_error("File can't be exported:\n"
                "No data or traces have different sizes" );

            return false;
        }
    }

    biosig_set_number_of_records(hdr, NRec);
    hdr->AS.bpb = 0;
    for (k = 0; k < numberOfChannels; ++k) {
        // TODO replace accessing fields of struct
        CHANNEL_TYPE *hc = hdr->CHANNEL+k;
        hc->SPR = hdr->SPR / hc->SPR;
        hc->bi  = hdr->AS.bpb;
        hdr->AS.bpb += hc->SPR * 8; /* its always double */
    }

	/***
	    build Event table for storing segment information
	    pre-allocate memory for even table
         ***/

        numberOfEvents *= 2;    // about two events per segment
        biosig_set_number_of_events(hdr, numberOfEvents);

    /* check whether all segments have same size */
    {
        char flag = (hdr->NS>0);
        size_t m, POS, pos;
        for (k=0; k < hdr->NS; ++k) {
            pos = Data[k].size();
            if (k==0)
                POS = pos;
            else
                flag &= (POS == pos);
        }
        for (m=0; flag && (m < Data[(size_t)0].size()); ++m) {
            for (k=0; k < biosig_get_number_of_channels(hdr); ++k) {
                pos = Data[k][m].size() * lround(Data[k][m].GetXScale()/Data.GetXScale());
                if (k==0)
                    POS = pos;
                else
                    flag &= (POS == pos);
            }
        }
        if (!flag) {
            destructHDR(hdr);
            throw std::runtime_error(
                    "File can't be exported:\n"
                    "Traces have different sizes or no channels found"
            );
            return false;
        }
    }

        size_t N=0;
        k=0;
        size_t pos = 0;
        for (m=0; m < (Data[k].size()); ++m) {
            if (pos > 0) {
                uint16_t typ=0x7ffe;
                uint32_t pos32=pos;
                uint16_t chn=0;
                uint32_t dur=0;
                // set break marker
                biosig_set_nth_event(hdr, N++, &typ, &pos32, &chn, &dur, NULL, NULL);
                /*
                // set annotation
                const char *Desc = Data[k][m].GetSectionDescription().c_str();
                 if (Desc != NULL && strlen(Desc)>0)
                    biosig_set_nth_event(hdr, N++, NULL, &pos32, &chn, &dur, NULL, Desc);   // TODO
                */
            }
            pos += Data[k][m].size() * lround(Data[k][m].GetXScale()/Data.GetXScale());
        }

        biosig_set_number_of_events(hdr, N);
        biosig_set_eventtable_samplerate(hdr, fs);
        sort_eventtable(hdr);

	/* convert data into GDF rawdata from  */
	biosig_data_type *rawdata = (biosig_data_type*)malloc(hdr->AS.bpb * hdr->NRec);

	for (k=0; k < numberOfChannels; ++k) {
        CHANNEL_TYPE *hc = biosig_get_channel(hdr, k);

        size_t m,n,len=0;
        for (m=0; m < Data[k].size(); ++m) {
            size_t div = lround(Data[k][m].GetXScale()/Data.GetXScale());
            size_t div2 = hdr->SPR/div;

            // fprintf(stdout,"k,m,div,div2: %i,%i,%i,%i\n",(int)k,(int)m,(int)div,(int)div2);  //
            for (n=0; n < Data[k][m].size(); ++n) {
                uint64_t val;
                double d = Data[k][m][n];
#if !defined(__MINGW32__) && !defined(_MSC_VER)
                val = htole64(*(uint64_t*)&d);
#else
                val = *(uint64_t*)&d;
#endif
                size_t p, spr = (len + n*div) / hdr->SPR;
                for (p=0; p < div2; p++)
                   *(uint64_t*)(rawdata + hc->bi + hdr->AS.bpb * spr + p*8) = val;
            }
            len += div*Data[k][m].size();
        }
    }

    /******************************
        write to file
    *******************************/
    std::string errorMsg("Exception while calling std::exportBiosigFile():\n");

    hdr = sopen( fName.c_str(), "w", hdr );
    if (serror2(hdr)) {
        errorMsg += biosig_get_errormsg(hdr);
        destructHDR(hdr);
        throw std::runtime_error(errorMsg.c_str());
        return false;
    }

    ifwrite(rawdata, hdr->AS.bpb, NRec, hdr);

    sclose(hdr);
    destructHDR(hdr);
    free(rawdata);

#else   // #ifndef __LIBBIOSIG2_H__


    HDRTYPE* hdr = constructHDR(Data.size(), 0);
    assert(hdr->NS == Data.size());

	/* Initialize all header parameters */
    hdr->TYPE = GDF;
    hdr->VERSION = 3.0;   // latest version

    struct tm t;
    char *str;
    str = strdup(Data.GetDate().c_str());
    t.tm_year = strtol(str,&str,10)-1900;
    t.tm_mon  = strtol(str+1,&str,10)-1;
    t.tm_mday = strtol(str+1,&str,10);

    str = strdup(Data.GetTime().c_str());
    t.tm_hour = strtol(str,&str,10);
    t.tm_min = strtol(str+1,&str,10);
    t.tm_sec = strtol(str+1,&str,10);

    hdr->T0   = tm_time2gdf_time(&t);

    const char *xunits = Data.GetXUnits().c_str();
#if (BIOSIG_VERSION_MAJOR > 0)
    uint16_t pdc = PhysDimCode(xunits);
#else
    uint16_t pdc = PhysDimCode((char*)xunits);
#endif
    if ((pdc & 0xffe0) == PhysDimCode("s")) {
        fprintf(stderr,"Stimfit exportBiosigFile: xunits [%s] has not proper units, assume [ms]\n",Data.GetXUnits().c_str());
        pdc = PhysDimCode("ms");
    }
    hdr->SampleRate = 1.0/(PhysDimScale(pdc) * Data.GetXScale());
    hdr->SPR  = 1;

    hdr->FLAG.UCAL = 0;
    hdr->FLAG.OVERFLOWDETECTION = 0;

    hdr->FILE.COMPRESSION = 0;

	/* Initialize all channel parameters */
    size_t k, m;
    for (k = 0; k < hdr->NS; ++k) {
        CHANNEL_TYPE *hc = hdr->CHANNEL+k;

        hc->PhysMin = -1e9;
        hc->PhysMax =  1e9;
        hc->DigMin  = -1e9;
        hc->DigMax  =  1e9;
        hc->Cal     =  1.0;
        hc->Off     =  0.0;

        /* Channel descriptions. */
        strncpy(hc->Label, Data[k].GetChannelName().c_str(), MAX_LENGTH_LABEL);
#if (BIOSIG_VERSION_MAJOR > 0)
        hc->PhysDimCode = PhysDimCode(Data[k].GetYUnits().c_str());
#else
        hc->PhysDimCode = PhysDimCode((char*)Data[k].GetYUnits().c_str());
#endif
        hc->OnOff      = 1;
        hc->LeadIdCode = 0;

        hc->TOffset  = 0.0;
        hc->Notch    = NAN;
        hc->LowPass  = NAN;
        hc->HighPass = NAN;
        hc->Impedance= NAN;

        hc->SPR    = hdr->SPR;
        hc->GDFTYP = 17; 	// double

        // each segment gets one marker, roughly
        hdr->EVENT.N += Data[k].size();

        size_t m,len = 0;
        for (len=0, m = 0; m < Data[k].size(); ++m) {
            unsigned div = lround(Data[k][m].GetXScale()/Data.GetXScale());
            hc->SPR = lcm(hc->SPR,div);  // sampling interval of m-th segment in k-th channel
            len += div*Data[k][m].size();
        }
        hdr->SPR = lcm(hdr->SPR, hc->SPR);

        if (k==0) {
            hdr->NRec = len;
        }
        else if ((size_t)hdr->NRec != len) {
            destructHDR(hdr);
            throw std::runtime_error("File can't be exported:\n"
                "No data or traces have different sizes" );

            return false;
        }
    }

    hdr->AS.bpb = 0;
    for (k = 0; k < hdr->NS; ++k) {
        CHANNEL_TYPE *hc = hdr->CHANNEL+k;
        hc->SPR = hdr->SPR / hc->SPR;
        hc->bi  = hdr->AS.bpb;
        hdr->AS.bpb += hc->SPR * 8; /* its always double */
    }

	/***
	    build Event table for storing segment information
	 ***/
	size_t N = hdr->EVENT.N * 2;    // about two events per segment
	hdr->EVENT.POS = (uint32_t*)realloc(hdr->EVENT.POS, N * sizeof(*hdr->EVENT.POS));
	hdr->EVENT.DUR = (uint32_t*)realloc(hdr->EVENT.DUR, N * sizeof(*hdr->EVENT.DUR));
	hdr->EVENT.TYP = (uint16_t*)realloc(hdr->EVENT.TYP, N * sizeof(*hdr->EVENT.TYP));
	hdr->EVENT.CHN = (uint16_t*)realloc(hdr->EVENT.CHN, N * sizeof(*hdr->EVENT.CHN));
#if (BIOSIG_VERSION >= 10500)
	hdr->EVENT.TimeStamp = (gdf_time*)realloc(hdr->EVENT.TimeStamp, N * sizeof(gdf_time));
#endif

    /* check whether all segments have same size */
    {
        char flag = (hdr->NS>0);
        size_t m, POS, pos;
        for (k=0; k < hdr->NS; ++k) {
            pos = Data[k].size();
            if (k==0)
                POS = pos;
            else
                flag &= (POS == pos);
        }
        for (m=0; flag && (m < Data[(size_t)0].size()); ++m) {
            for (k=0; k < hdr->NS; ++k) {
                pos = Data[k][m].size() * lround(Data[k][m].GetXScale()/Data.GetXScale());
                if (k==0)
                    POS = pos;
                else
                    flag &= (POS == pos);
            }
        }
        if (!flag) {
            destructHDR(hdr);
            throw std::runtime_error(
                    "File can't be exported:\n"
                    "Traces have different sizes or no channels found"
            );
            return false;
        }
    }

        N=0;
        k=0;
        size_t pos = 0;
        for (m=0; m < (Data[k].size()); ++m) {
            if (pos > 0) {
                // start of new segment after break
                hdr->EVENT.POS[N] = pos;
                hdr->EVENT.TYP[N] = 0x7ffe;
                hdr->EVENT.CHN[N] = 0;
                hdr->EVENT.DUR[N] = 0;
                N++;
            }
#if 0
            // event description
            hdr->EVENT.POS[N] = pos;
            FreeTextEvent(hdr, N, "myevent");
            //FreeTextEvent(hdr, N, Data[k][m].GetSectionDescription().c_str()); // TODO
            hdr->EVENT.CHN[N] = k;
            hdr->EVENT.DUR[N] = 0;
            N++;
#endif
            pos += Data[k][m].size() * lround(Data[k][m].GetXScale()/Data.GetXScale());
        }

        hdr->EVENT.N = N;
        hdr->EVENT.SampleRate = hdr->SampleRate;

        sort_eventtable(hdr);

	/* convert data into GDF rawdata from  */
	hdr->AS.rawdata = (uint8_t*)realloc(hdr->AS.rawdata, hdr->AS.bpb*hdr->NRec);
	for (k=0; k < hdr->NS; ++k) {
        CHANNEL_TYPE *hc = hdr->CHANNEL+k;

        size_t m,n,len=0;
        for (m=0; m < Data[k].size(); ++m) {
            size_t div = lround(Data[k][m].GetXScale()/Data.GetXScale());
            size_t div2 = hdr->SPR/div;

            // fprintf(stdout,"k,m,div,div2: %i,%i,%i,%i\n",(int)k,(int)m,(int)div,(int)div2);  //
            for (n=0; n < Data[k][m].size(); ++n) {
                uint64_t val;
                double d = Data[k][m][n];
#if !defined(__MINGW32__) && !defined(_MSC_VER)
                val = htole64(*(uint64_t*)&d);
#else
                val = *(uint64_t*)&d;
#endif
                size_t p, spr = (len + n*div) / hdr->SPR;
                for (p=0; p < div2; p++)
                   *(uint64_t*)(hdr->AS.rawdata + hc->bi + hdr->AS.bpb * spr + p*8) = val;
            }
            len += div*Data[k][m].size();
        }
    }

    /******************************
        write to file
    *******************************/
    std::string errorMsg("Exception while calling std::exportBiosigFile():\n");

    hdr = sopen( fName.c_str(), "w", hdr );
#if (BIOSIG_VERSION > 10400)
    if (serror2(hdr)) {
        errorMsg += hdr->AS.B4C_ERRMSG;
#else
    if (serror()) {
	    errorMsg += B4C_ERRMSG;
#endif
        destructHDR(hdr);
        throw std::runtime_error(errorMsg.c_str());
        return false;
    }

    ifwrite(hdr->AS.rawdata, hdr->AS.bpb, hdr->NRec, hdr);

    sclose(hdr);
    destructHDR(hdr);
#endif

    return true;
}

