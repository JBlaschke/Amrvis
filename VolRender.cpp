
//
// $Id: VolRender.cpp,v 1.51 2004-09-30 20:03:38 vince Exp $
//

// ---------------------------------------------------------------
// VolRender.cpp
// ---------------------------------------------------------------
#include "VolRender.H"
#include "DataServices.H"
#include "GlobalUtilities.H"
#include "ParallelDescriptor.H"

#include <iostream>
#include <cstdlib>
using std::cerr;
using std::cout;
using std::endl;
using std::max;
using std::min;
#include <ctime>

#include <unistd.h>
#include <fcntl.h>

extern Real RadToDeg(Real angle);
extern Real DegToRad(Real angle);

#define CheckVP(vpret, n)  \
	  if(vpret != VP_OK) { \
            cerr << "VolPack error " << n << ":  " \
		 << vpGetErrorString(vpGetError(vpc)) << endl; \
            exit(-4); \
	  }


// -------------------------------------------------------------------
VolRender::VolRender(const Array<Box> &drawdomain, int mindrawnlevel,
		     int maxdrawnlevel, Palette *paletteptr,
                     const string &asLightFileName)
{
  bDrawAllBoxes = false;
  minDrawnLevel = mindrawnlevel;
  maxDataLevel = maxdrawnlevel;
  drawnDomain = drawdomain;
  vpDataValid     = false;
  swfDataValid    = false;
  swfDataAllocated = false;
  palettePtr = paletteptr;
  preClassify = true;

  // these are the defaults
  ambientMat = 0.28;
  diffuseMat = 0.35;
  specularMat = 0.39;
  shinyMat = 10.0;
  minRayOpacity = 0.05;
  maxRayOpacity = 0.95;

  Real ambient, diffuse, specular, shiny, minray, maxray;
  bool bFileOk = AVGlobals::ReadLightingFile(asLightFileName, ambient, diffuse,
                                             specular, shiny, minray, maxray);
  
  if(bFileOk) {
    if(0.0 > ambient || ambient > 1.0) {
      cerr << "Error:  ambient value must be in the range (0.0, 1.0)." << endl;
    } else {
      ambientMat = ambient;
    }
    if(0.0 > diffuse || diffuse > 1.0) {
      cerr << "Error:  diffuse value must be in the range (0.0, 1.0)." << endl;
    } else {
      diffuseMat = diffuse;
    }
    if(0.0 > specular || specular > 1.0) {
      cerr << "Error:  specular value must be in the range (0.0, 1.0)." << endl;
    } else {
      specularMat = specular;
    }

    shinyMat = shiny;

    if(0.0 > minray || minray > 1.0) {
      cerr << "Error:  minray value must be in the range (0.0, 1.0)." << endl;
    } else {
      minRayOpacity = minray;
    }
    if(0.0 > maxray || maxray > 1.0) {
      cerr << "Error:  maxray value must be in the range (0.0, 1.0)." << endl;
    } else {
      maxRayOpacity = maxray;
    }
  }


  volData = NULL;
  voxelFields = 3;

  //voxel field variables:
  static RawVoxel* dummy_voxel;
  normalField = 0;
  normalOffset = vpFieldOffset(dummy_voxel, normal);
  normalSize = 2;		// sizeof(short); on a t3e this will want to be 2
  normalMax = VP_NORM_MAX;

  densityField = 1;
  densityOffset = vpFieldOffset(dummy_voxel, density);
  densitySize = sizeof(unsigned char);
  densityMax = 255;


  gradientField = 2;
  gradientOffset = vpFieldOffset(dummy_voxel, gradient);
  gradientSize = sizeof(unsigned char);
  gradientMax = 255;

  lightingModel = true;
  paletteSize = 256;

  if(ParallelDescriptor::IOProcessor()) {
    // must initialize vpc before calling ReadTransferFile
    vpc = vpCreateContext();

    maxDenRampPts   = densityMax  + 1;
    maxShadeRampPts = normalMax   + 1;
    density_ramp.resize(maxDenRampPts);
    shade_table.resize(maxShadeRampPts);
    value_shade_table.resize(paletteSize);

    maxGradRampPts = gradientMax + 1;
    gradient_ramp.resize(maxGradRampPts);

    SetProperties();

    rows   = drawnDomain[maxDataLevel].length(XDIR);
    cols   = drawnDomain[maxDataLevel].length(YDIR);
    planes = drawnDomain[maxDataLevel].length(ZDIR);

    // --- describe the layout of the volume
    vpResult vpret = vpSetVolumeSize(vpc, rows, cols, planes);
    CheckVP(vpret, 1);

  }
  bVolRenderDefined = true;
}  // end VolRender()


// -------------------------------------------------------------------
VolRender::~VolRender() {
  BL_ASSERT(bVolRenderDefined);
  if(ParallelDescriptor::IOProcessor()) {
    //cout << "_in VolRender::~VolRender():  volData = " << volData << endl;
    vpDestroyContext(vpc);
    //cout << "_after vpDestroyContext:  volData = " << volData << endl;
    delete [] volData;
    //cout << "_after delete [] volData:  volData = " << volData << endl;
    if(swfDataAllocated) {
      delete [] swfData;
    }
  }
}


// -------------------------------------------------------------------
bool VolRender::AllocateSWFData() {
  BL_ASSERT(bVolRenderDefined);

  // --- create the big array
  swfDataSize = drawnDomain[maxDataLevel].numPts();
  cout << "swfData box size = "
       << drawnDomain[maxDataLevel] << "  " << swfDataSize << endl;

  swfData = new unsigned char[swfDataSize];
  if(swfData == NULL) {
    cerr << "Error in AmrPicture::ChangeDerived:  could not allocate "
         << swfDataSize << " bytes for swfData." << endl;
    swfDataAllocated = false;
  } else {
    swfDataAllocated = true;
  }
  return swfDataAllocated;
}


// -------------------------------------------------------------------
void VolRender::MakeSWFData(DataServices *dataServicesPtr,
			    Real rDataMin, Real rDataMax,
			    const string &derivedName,
			    int iPaletteStart, int iPaletteEnd,
			    int iBlackIndex, int iWhiteIndex,
			    int iColorSlots, const bool bdrawboxes)
{
  BL_ASSERT(bVolRenderDefined);
  
  if(swfDataValid) {
    return;
  }
  
  bDrawAllBoxes = bdrawboxes;

  if( ! swfDataAllocated) {
    if(ParallelDescriptor::IOProcessor()) {
      swfDataAllocated = AllocateSWFData();
    } else {
      swfDataAllocated = true;
    }
  }
  
  swfDataValid = true;
  clock_t time0 = clock();
  
  int maxDrawnLevel(maxDataLevel);
  Box grefbox;

  Box swfDataBox(drawnDomain[maxDrawnLevel]);

  FArrayBox swfFabData;
  if(ParallelDescriptor::IOProcessor()) {
    swfFabData.resize(swfDataBox, 1);
  }
  
  DataServices::Dispatch(DataServices::FillVarOneFab, dataServicesPtr,
                         (void *) &swfFabData,
			 (void *) &swfDataBox,
			 maxDrawnLevel,
			 (void *) &derivedName);
  
  if(ParallelDescriptor::IOProcessor()) {
    Real gmin(rDataMin);
    Real gmax(rDataMax);
    Real globalDiff(gmax - gmin);
    Real oneOverGDiff;
    if(globalDiff < FLT_MIN) {
      oneOverGDiff = 0.0;  // so we dont divide by zero
    } else {
      oneOverGDiff = 1.0 / globalDiff;
    }
    int cSlotsAvail(iColorSlots - 1);
    
    cout << "Filling swfFabData..." << endl;
    
    // copy data into swfData and change to chars
    Real dat;
    char chardat;
    Real *dataPoint = swfFabData.dataPtr();
    
    int sindexbase;
    int srows   = swfDataBox.length(XDIR);
    int scols   = swfDataBox.length(YDIR);
    //int splanes = swfDataBox.length(ZDIR);
    int scolssrowstmp = scols*srows;
    int sstartr = swfDataBox.smallEnd(XDIR);
    //int sstartc = swfDataBox.smallEnd(YDIR);
    int sstartp = swfDataBox.smallEnd(ZDIR);
    //int sendr   = swfDataBox.bigEnd(XDIR);
    int sendc   = swfDataBox.bigEnd(YDIR);
    //int sendp   = swfDataBox.bigEnd(ZDIR);
    
    Box gbox(swfDataBox);
    Box goverlap(gbox & drawnDomain[maxDrawnLevel]);
    
    int gstartr = gbox.smallEnd(XDIR);
    int gstartc = gbox.smallEnd(YDIR);
    int gstartp = gbox.smallEnd(ZDIR);
    
    int gostartr = goverlap.smallEnd(XDIR) - gstartr;
    int gostartc = goverlap.smallEnd(YDIR) - gstartc;
    int gostartp = goverlap.smallEnd(ZDIR) - gstartp;
    int goendr   = goverlap.bigEnd(XDIR)   - gstartr;
    int goendc   = goverlap.bigEnd(YDIR)   - gstartc;
    int goendp   = goverlap.bigEnd(ZDIR)   - gstartp;
    
    int grows   = gbox.length(XDIR);
    int gcols   = gbox.length(YDIR);
    //int gplanes = gbox.length(ZDIR);
    
    int gcolsgrowstmp(gcols * grows);
    int gpgcgrtmp, gcgrowstmp;
    int gprev;
    for(int gp(gostartp); gp <= goendp; ++gp) {
      gpgcgrtmp = gp * gcolsgrowstmp;
      for(int gc(gostartc); gc <= goendc; ++gc) {
        gcgrowstmp = gpgcgrtmp + gc * grows;
        for(int gr(gostartr); gr <= goendr; ++gr) {
          //dat = dataPoint[(gp * gcols * grows) + (gc * grows) + gr];  // works
          dat = dataPoint[gcgrowstmp + gr];
          dat = max(dat,gmin); // clip data if out of range
          dat = min(dat,gmax);
          chardat = (char) (((dat - gmin) * oneOverGDiff) * cSlotsAvail);
          chardat += (char) iPaletteStart;
	  gprev = gostartp + goendp - gp;
          sindexbase =
            //(((gp + gstartp) - sstartp) * scolssrowstmp) +
            (((gprev + gstartp) - sstartp) * scolssrowstmp) +
            ((sendc - ((gc + gstartc))) * srows) +  // check this
            ((gr + gstartr) - sstartr);
          
          swfData[sindexbase] = chardat;
        }
      }
    }  // end for(gp...)

                                                // ---------------- VolumeBoxes
    bool bDrawVolumeBoxes(AVGlobals::GetBoxColor() > -1);  // need to limit
                                                           // to palmaxindex
    if(bDrawVolumeBoxes) {
      int edger, edgec, edgep;
      int volumeBoxColor(AVGlobals::GetBoxColor());
      int gr, gc, gp, sr, sc, sp, sindex;
      AmrData &amrData = dataServicesPtr->AmrDataRef();

     if(bDrawAllBoxes) {
      for(int lev(minDrawnLevel); lev <= maxDrawnLevel; ++lev) {
        int crr(AVGlobals::CRRBetweenLevels(lev, maxDrawnLevel,
	        amrData.RefRatio()));
	const BoxArray &gridBoxes = amrData.boxArray(lev);
	for(int iGrid(0); iGrid < gridBoxes.size(); ++iGrid) {
          gbox = gridBoxes[iGrid];
	  // grow high end by one to eliminate overlap
	  //gbox.growHi(XDIR, 1);
	  //gbox.growHi(YDIR, 1);
	  //gbox.growHi(ZDIR, 1);
	  //
          Box goverlap(gbox & drawnDomain[lev]);
          grefbox = goverlap;
          grefbox.refine(crr);

	  int gprev;
          int gstartr(gbox.smallEnd(XDIR));
          int gstartc(gbox.smallEnd(YDIR));
          int gstartp(gbox.smallEnd(ZDIR));

          int gostartr(goverlap.smallEnd(XDIR) - gstartr);
          int gostartc(goverlap.smallEnd(YDIR) - gstartc);
          int gostartp(goverlap.smallEnd(ZDIR) - gstartp);
          int goendr(goverlap.bigEnd(XDIR)   - gstartr);
          int goendc(goverlap.bigEnd(YDIR)   - gstartc);
          int goendp(goverlap.bigEnd(ZDIR)   - gstartp);

          grows = gbox.length(XDIR);
          gcols = gbox.length(YDIR);

        if(crr != 1) {
          int gcolsgrowstmp(gcols * grows);
	  int ddsez(drawnDomain[lev].smallEnd(ZDIR));
	  int ddbez(drawnDomain[lev].bigEnd(ZDIR));
          for(gp = gostartp; gp <= goendp; ++gp) {
            gprev = ddsez + ddbez - (gp + gstartp);
	    if(gp == gostartp || gp == goendp) {
              edgep = 1;
	    } else {
              edgep = 0;
	    }
            gpgcgrtmp = gp * gcolsgrowstmp;
            for(gc = gostartc; gc <= goendc; ++gc) {
	      if(gc == gostartc || gc == goendc) {
                edgec = 1;
	      } else {
                edgec = 0;
	      }
              for(gr = gostartr; gr <= goendr; ++gr) {
		if(gr == gostartr || gr == goendr) {
                  edger = 1;
		} else {
                  edger = 0;
		}
                //sindexbase = (((gp + gstartp) * crr - sstartp) * scolssrowstmp) +
                sindexbase = (((gprev) * crr - sstartp) * scolssrowstmp) +
                             ((sendc - ((gc + gstartc) * crr)) * srows) +
                             ((gr + gstartr) * crr - sstartr);

              if((edger + edgec + edgep) > 1) {
                // (possibly) draw boxes into dataset
                int onEdger, onEdgec, onEdgep;

                for(sp = 0; sp < crr; ++sp) {
		  if((gp==gostartp && sp==0) || (gp==goendp && sp == (crr-1))) {
                    onEdgep = 1;
		  } else {
                    onEdgep = 0;
		  }
                  for(sc = 0; sc < crr; ++sc) {
		    if((gc==gostartc && sc==0) || (gc==goendc && sc == (crr-1))) {
                      onEdgec = 1;
		    } else {
                      onEdgec = 0;
		    }
                    for(sr = 0; sr < crr; ++sr) {
		      if((gr==gostartr && sr==0) || (gr==goendr && sr == (crr-1))) {
                        onEdger = 1;
		      } else {
                        onEdger = 0;
		      }
                      if((onEdger + onEdgec + onEdgep) > 1) {
                        sindex = sindexbase + ((sp * scolssrowstmp) -
					       (sc * srows) + sr);
                        swfData[sindex] = volumeBoxColor;
                      }

                    }
                  }
                }  // end for(sp...)
              }
              }
            }
          }  // end for(gp...)

        } else {  // crr == 1
	  int ddsez(drawnDomain[lev].smallEnd(ZDIR));
	  int ddbez(drawnDomain[lev].bigEnd(ZDIR));
          for(gp = gostartp; gp <= goendp; ++gp) {
            gprev = ddsez + ddbez - (gp + gstartp);
	    if(gp == gostartp || gp == goendp) {
              edgep = 1;
	    } else {
              edgep = 0;
	    }
            for(gc = gostartc; gc <= goendc; ++gc) {
	      if(gc == gostartc || gc == goendc) {
                edgec = 1;
	      } else {
                edgec = 0;
	      }
              for(gr = gostartr; gr <= goendr; ++gr) {
		if(gr == gostartr || gr == goendr) {
                  edger = 1;
		} else {
                  edger = 0;
		}
                if((edger + edgec + edgep) > 1) {
                  sindexbase =
                      //(((gp + gstartp) - sstartp) * scolssrowstmp) +
                      ((gprev - sstartp) * scolssrowstmp) +
                      ((sendc - ((gc + gstartc))) * srows) +
                      ((gr + gstartr) - sstartr);
                  swfData[sindexbase] = volumeBoxColor;
                }
              }
            }
          }  // end for(gp...)
        }  // end if(crr...)

        }  // end for(iGrid...)
      }  // end for(lev...)

     } else {  // only draw the boundingbox

        int lev = minDrawnLevel;
        int crr(AVGlobals::CRRBetweenLevels(lev, maxDrawnLevel,
	        amrData.RefRatio()));
          gbox = drawnDomain[lev];
          Box goverlap(gbox & drawnDomain[lev]);
          grefbox = goverlap;
          grefbox.refine(crr);

          int gstartr(gbox.smallEnd(XDIR));
          int gstartc(gbox.smallEnd(YDIR));
          int gstartp(gbox.smallEnd(ZDIR));

          int gostartr(goverlap.smallEnd(XDIR) - gstartr);
          int gostartc(goverlap.smallEnd(YDIR) - gstartc);
          int gostartp(goverlap.smallEnd(ZDIR) - gstartp);
          int goendr(goverlap.bigEnd(XDIR)   - gstartr);
          int goendc(goverlap.bigEnd(YDIR)   - gstartc);
          int goendp(goverlap.bigEnd(ZDIR)   - gstartp);

          grows = gbox.length(XDIR);
          gcols = gbox.length(YDIR);

        if(crr != 1) {
          int gcolsgrowstmp(gcols * grows);
          for(gp = gostartp; gp <= goendp; ++gp) {
            if(gp == gostartp || gp == goendp) {
              edgep = 1;
            } else {
              edgep = 0;
            }
            gpgcgrtmp = gp * gcolsgrowstmp;
            for(gc = gostartc; gc <= goendc; ++gc) {
              if(gc == gostartc || gc == goendc) {
                edgec = 1;
              } else {
                edgec = 0;
              }
              for(gr = gostartr; gr <= goendr; ++gr) {
                if(gr == gostartr || gr == goendr) {
                  edger = 1;
                } else {
                  edger = 0;
                }
                sindexbase = (((gp + gstartp) * crr - sstartp) * scolssrowstmp) +
                             ((sendc - ((gc + gstartc) * crr)) * srows) +
                             ((gr + gstartr) * crr - sstartr);

              if((edger + edgec + edgep) > 1) {
                // (possibly) draw boxes into dataset
                int onEdger, onEdgec, onEdgep;

                for(sp = 0; sp < crr; ++sp) {
                  if((gp==gostartp && sp==0) || (gp==goendp && sp == (crr-1))) {
                    onEdgep = 1;
                  } else {
                    onEdgep = 0;
                  }
                  for(sc = 0; sc < crr; ++sc) {
                    if((gc==gostartc && sc==0) || (gc==goendc && sc == (crr-1))) {
                      onEdgec = 1;
                    } else {
                      onEdgec = 0;
                    }
                    for(sr = 0; sr < crr; ++sr) {
                      if((gr==gostartr && sr==0) || (gr==goendr && sr == (crr-1))) {
                        onEdger = 1;
                      } else {
                        onEdger = 0;
                      }
                      if((onEdger + onEdgec + onEdgep) > 1) {
                        sindex = sindexbase + ((sp * scolssrowstmp) -
                                               (sc * srows) + sr);
                        swfData[sindex] = volumeBoxColor;
                      }

                    }
                  }
                }  // end for(sp...)
              }
              }
            }
          }  // end for(gp...)

        } else {  // crr == 1

          int gcolsgrowstmp(gcols * grows);
          for(gp = gostartp; gp <= goendp; ++gp) {
            if(gp == gostartp || gp == goendp) {
              edgep = 1;
            } else {
              edgep = 0;
            }
            gpgcgrtmp = gp * gcolsgrowstmp;
            for(gc = gostartc; gc <= goendc; ++gc) {
              if(gc == gostartc || gc == goendc) {
                edgec = 1;
              } else {
                edgec = 0;
              }
              for(gr = gostartr; gr <= goendr; ++gr) {
                if(gr == gostartr || gr == goendr) {
                  edger = 1;
                } else {
                  edger = 0;
                }
                if((edger + edgec + edgep) > 1) {
                  sindexbase =
                      (((gp + gstartp) - sstartp) * scolssrowstmp) +
                      ((sendc - ((gc + gstartc))) * srows) +
                      ((gr + gstartr) - sstartr);
                  swfData[sindexbase] = volumeBoxColor;
                }
              }
            }
          }  // end for(gp...)
        }  // end if(crr...)

     }  // end if(bDrawAllBoxes)

    }  // end if(bDrawVolumeBoxes)


  }  // end if(ParallelDescriptor::IOProcessor())


  
  // fix up cartgrid body
  AmrData &amrData = dataServicesPtr->AmrDataRef();
  const string vfracName = "vfrac";
  if(amrData.CartGrid() && derivedName != vfracName) {
    // reuse swfFabData
    DataServices::Dispatch(DataServices::FillVarOneFab, dataServicesPtr,
                           (void *) &swfFabData,
			   (void *) &swfDataBox,
			   maxDrawnLevel,
			   (void *) &vfracName);

    if(ParallelDescriptor::IOProcessor()) {
      char bodyColor = (char) palettePtr->BodyIndex();
      Real *dataPoint = swfFabData.dataPtr();
      Real vfeps = amrData.VfEps(maxDrawnLevel);
      int sindexbase;
      int srows   = swfDataBox.length(XDIR);
      int scols   = swfDataBox.length(YDIR);
      int scolssrowstmp = scols*srows;
      int sstartr = swfDataBox.smallEnd(XDIR);
      int sstartp = swfDataBox.smallEnd(ZDIR);
      int sendc   = swfDataBox.bigEnd(YDIR);
    
      Box gbox(swfDataBox);
      Box goverlap(gbox & drawnDomain[maxDrawnLevel]);
    
      int gstartr = gbox.smallEnd(XDIR);
      int gstartc = gbox.smallEnd(YDIR);
      int gstartp = gbox.smallEnd(ZDIR);
    
      int gostartr = goverlap.smallEnd(XDIR) - gstartr;
      int gostartc = goverlap.smallEnd(YDIR) - gstartc;
      int gostartp = goverlap.smallEnd(ZDIR) - gstartp;
      int goendr   = goverlap.bigEnd(XDIR)   - gstartr;
      int goendc   = goverlap.bigEnd(YDIR)   - gstartc;
      int goendp   = goverlap.bigEnd(ZDIR)   - gstartp;
    
      int grows   = gbox.length(XDIR);
      int gcols   = gbox.length(YDIR);
    
      int gcolsgrowstmp(gcols * grows);
      int gpgcgrtmp, gcgrowstmp;
      int gprev;
      for(int gp(gostartp); gp <= goendp; ++gp) {
        gpgcgrtmp = gp*gcolsgrowstmp;
        for(int gc(gostartc); gc <= goendc; ++gc) {
          gcgrowstmp = gpgcgrtmp + gc*grows;
          for(int gr(gostartr); gr <= goendr; ++gr) {
            //dat = dataPoint[(gp*gcols*grows)+(gc*grows)+gr];  // works
            if(dataPoint[gcgrowstmp + gr] < vfeps) {  // body
	      gprev = gostartp + goendp - gp;
              sindexbase =
                (((gprev+gstartp)-sstartp) * scolssrowstmp) +
                ((sendc-((gc+gstartc))) * srows) +  // check this
                ((gr+gstartr)-sstartr);
          
              swfData[sindexbase] = bodyColor;
	    }
          }
        }
      }  // end for(gp...)

    }  // end if(ioproc)
  }

  if(ParallelDescriptor::IOProcessor()) {
    cout << endl;
    cout << "--------------- make swfData time = "
         << ((clock()-time0)/1000000.0) << endl;
  }

}  // end MakeSWFData(...)


// -------------------------------------------------------------------
void VolRender::WriteSWFData(const string &filenamebase, bool SWFLight) {
    cout << "VolRender::WriteSWFData" << endl;
    BL_ASSERT(bVolRenderDefined);
    if(ParallelDescriptor::IOProcessor()) {
        cout << "vpClassify Scalars..." << endl;           // --- classify
        clock_t time0 = clock();
        vpResult vpret;
        bool PCtemp(preClassify);
        preClassify = true;
        // here set lighting or value model
        bool bLMtemp(lightingModel);
        lightingModel = SWFLight;
 
        MakeVPData();
        
        preClassify = PCtemp;
        lightingModel = bLMtemp;
   

  cout << "----- make vp data time = " << ((clock() - time0)/1000000.0) << endl;
  string filename = "swf.";
  filename += filenamebase;
  filename += (SWFLight ? ".lt" : ".val" );
  filename += ".vpdat";
  cout << "----- storing classified volume into file:  " << filename << endl;
#ifndef S_IRUSR  /* the T3E does not define this */
#define S_IRUSR 0000400
#endif
#ifndef S_IWUSR  /* the T3E does not define this */
#define S_IWUSR 0000200
#endif
#ifndef S_IRGRP  /* the T3E does not define this */
#define S_IRGRP 0000040
#endif
    int fp = open(filename.c_str(), O_CREAT | O_WRONLY,
		  S_IRUSR | S_IWUSR | S_IRGRP);
    vpret = vpStoreClassifiedVolume(vpc, fp);
    CheckVP(vpret, 4);
    close(fp);
  }
}


// -------------------------------------------------------------------
void VolRender::InvalidateSWFData() {
  swfDataValid = false;
}


// -------------------------------------------------------------------
void VolRender::InvalidateVPData() {
  vpDataValid = false;
}


// -------------------------------------------------------------------
void VolRender::SetLightingModel(bool lightOn) {
  if(lightingModel == lightOn) {
    return;
  }
  lightingModel = lightOn;
  if(lightingModel == true) {
    vpSetVoxelField(vpc, normalField, normalSize, normalOffset, maxShadeRampPts-1);
  } else {  // value model
    vpSetVoxelField(vpc, normalField, normalSize, normalOffset, paletteSize-1);
  }
}


// -------------------------------------------------------------------
void VolRender::SetPreClassifyAlgorithm(bool pC) {
  preClassify = pC;
}


// -------------------------------------------------------------------
void VolRender::SetImage(unsigned char *image_data, int width, int height,
                         int pixel_type)
{
    vpSetImage(vpc, image_data, width, height, width, pixel_type);
}


// -------------------------------------------------------------------
void VolRender::MakePicture(Real mvmat[4][4], Real Length, int width,
                            int height)
{
    vpCurrentMatrix(vpc, VP_MODEL);
    vpIdentityMatrix(vpc);
#ifdef BL_USE_FLOAT
    double dmvmat[4][4];
    for(int i(0); i < 4; ++i) {
      for(int j(0); j < 4; ++j) {
	dmvmat[i][j] = (double) mvmat[i][j];
      }
    }
    vpSetMatrix(vpc, dmvmat);
#else
    vpSetMatrix(vpc, mvmat);
#endif
    vpCurrentMatrix(vpc, VP_PROJECT);
    vpIdentityMatrix(vpc);
    vpLen = Length;
    if(width < height) {    // undoes volpacks aspect ratio scaling
        vpWindow(vpc, VP_PARALLEL, 
                 -Length*vpAspect, Length*vpAspect,
                 -Length, Length,
                 -Length, Length);
    } else {
        vpWindow(vpc, VP_PARALLEL, 
                 -Length, Length,
                 -Length*vpAspect, Length*vpAspect,
                 -Length, Length);
    }
    vpResult vpret;
    
    if(lightingModel) {
        vpret = vpShadeTable(vpc);
        CheckVP(vpret, 12);
    }
    
    if(preClassify) {
      vpret = vpRenderClassifiedVolume(vpc);   // --- render
      CheckVP(vpret, 11);
    } else {
      vpret = vpClassifyVolume(vpc);  // - classify and then render
      CheckVP(vpret, 11.1);
      vpret = vpRenderRawVolume(vpc);
      CheckVP(vpret, 11.2);
    }
}

// -------------------------------------------------------------------
void VolRender::MakeVPData() {
  BL_ASSERT(bVolRenderDefined);
  if(ParallelDescriptor::IOProcessor()) {
    clock_t time0 = clock();
    
    vpDataValid = true;
    
    cout << "vpClassifyScalars..." << endl;           // --- classify
    
    vpSetd(vpc, VP_MIN_VOXEL_OPACITY, minRayOpacity);
    vpSetd(vpc, VP_MAX_RAY_OPACITY,   maxRayOpacity);

    vpResult vpret;
    if(preClassify) {
      if(lightingModel) {
        vpret = vpClassifyScalars(vpc, swfData, swfDataSize,
                                  densityField, gradientField, normalField);
        CheckVP(vpret, 6);
      } else {  // value model
        // the classification and loading of the value model
        delete [] volData;
        volData = new RawVoxel[swfDataSize]; // volpack will delete this
        int xStride(sizeof(RawVoxel));
        int yStride(drawnDomain[maxDataLevel].length(XDIR) * sizeof(RawVoxel));
        int zStride(drawnDomain[maxDataLevel].length(XDIR) *
                    drawnDomain[maxDataLevel].length(YDIR) * sizeof(RawVoxel));
        vpret = vpSetRawVoxels(vpc, volData, swfDataSize * sizeof(RawVoxel),
                               xStride, yStride, zStride);
        CheckVP(vpret, 9.4);
        for(int vindex(0); vindex < swfDataSize; ++vindex) {
          volData[vindex].normal  = swfData[vindex];
          volData[vindex].density = swfData[vindex];
        }
        
        vpret = vpClassifyVolume(vpc);
        CheckVP(vpret, 9.5);
        
      }
    } else {   // load the volume data and precompute the minmax octree
      if(lightingModel) {
        delete [] volData;
        volData = new RawVoxel[swfDataSize]; 
        int xStride(sizeof(RawVoxel));
        int yStride(drawnDomain[maxDataLevel].length(XDIR) * sizeof(RawVoxel));
        int zStride(drawnDomain[maxDataLevel].length(XDIR) *
                    drawnDomain[maxDataLevel].length(YDIR) * sizeof(RawVoxel));
        vpret = vpSetRawVoxels(vpc, volData, swfDataSize * sizeof(RawVoxel),
                               xStride, yStride, zStride);
        CheckVP(vpret, 9.45);
        vpret = vpVolumeNormals(vpc, swfData, swfDataSize,
                                densityField, gradientField, normalField);
        CheckVP(vpret, 6.1);
        
      } else {  // value model
        delete [] volData;
        volData = new RawVoxel[swfDataSize]; 
        int xStride(sizeof(RawVoxel));
        int yStride(drawnDomain[maxDataLevel].length(XDIR) * sizeof(RawVoxel));
        int zStride(drawnDomain[maxDataLevel].length(XDIR) *
                    drawnDomain[maxDataLevel].length(YDIR) * sizeof(RawVoxel));
        vpret = vpSetRawVoxels(vpc, volData, swfDataSize * sizeof(RawVoxel),
                               xStride, yStride, zStride);
        CheckVP(vpret, 9.4);
        for(int vindex(0); vindex < swfDataSize; ++vindex) {
          volData[vindex].normal  = swfData[vindex];
          volData[vindex].density = swfData[vindex];
        }
      }     
      vpret = vpMinMaxOctreeThreshold(vpc, DENSITY_PARAM, 
                                      OCTREE_DENSITY_THRESH);
      CheckVP(vpret, 9.41);
      
      if(classifyFields == 2) {
        vpret = vpMinMaxOctreeThreshold(vpc, GRADIENT_PARAM, 
                                        OCTREE_GRADIENT_THRESH);
        CheckVP(vpret, 9.42);
      }
      
      vpret = vpCreateMinMaxOctree(vpc, 1, OCTREE_BASE_NODE_SIZE);
      CheckVP(vpret, 9.43);
    }
    
    // --- set the shading parameters

    if(lightingModel) {
      vpret = vpSetLookupShader(vpc, 1, 1, normalField, shade_table.dataPtr(),
                              maxShadeRampPts * sizeof(float), 0, NULL, 0);
      CheckVP(vpret, 7);

      vpSetMaterial(vpc, VP_MATERIAL0, VP_AMBIENT, VP_BOTH_SIDES,
                    ambientMat, ambientMat, ambientMat);//0.28, 0.28, 0.28);
      vpSetMaterial(vpc, VP_MATERIAL0, VP_DIFFUSE, VP_BOTH_SIDES, 
                    diffuseMat, diffuseMat, diffuseMat);//0.35, 0.35, 0.35);
      vpSetMaterial(vpc, VP_MATERIAL0, VP_SPECULAR, VP_BOTH_SIDES, 
                    specularMat, specularMat, specularMat);//0.39, 0.39, 0.39);
      vpSetMaterial(vpc, VP_MATERIAL0, VP_SHINYNESS, VP_BOTH_SIDES, 
                    shinyMat, 0.0, 0.0);//10.0,  0.0, 0.0);
  
      vpSetLight(vpc, VP_LIGHT0, VP_DIRECTION, 0.3, 0.3, 1.0);
      vpSetLight(vpc, VP_LIGHT0, VP_COLOR, 1.0, 1.0, 1.0);
      vpEnable(vpc, VP_LIGHT0, 1);
        
      vpSeti(vpc, VP_CONCAT_MODE, VP_CONCAT_LEFT);
        
      // --- compute shading lookup table
      vpret = vpShadeTable(vpc);
      CheckVP(vpret, 8);

    } else {  // value model
      BL_ASSERT(palettePtr != NULL);
      for(int sn(0); sn < paletteSize; ++sn) {
        value_shade_table[sn] = (float) sn;
      }
      value_shade_table[0] = (float) AVGlobals::MaxPaletteIndex();
     
      float maxf(0.0);
      float minf(1000000.0);
      for(int ijk(0); ijk < paletteSize; ++ijk) {
        maxf = max(maxf, value_shade_table[ijk]);
        minf = min(minf, value_shade_table[ijk]);
      }
      
      vpret = vpSetLookupShader(vpc, 1, 1, normalField, 
                                value_shade_table.dataPtr(),
                                paletteSize * sizeof(float), 0, NULL, 0);
      CheckVP(vpret, 9);
      
      vpSetMaterial(vpc, VP_MATERIAL0, VP_AMBIENT, VP_BOTH_SIDES,
                    ambientMat, ambientMat, ambientMat);//0.28, 0.28, 0.28);
      vpSetMaterial(vpc, VP_MATERIAL0, VP_DIFFUSE, VP_BOTH_SIDES, 
                    diffuseMat, diffuseMat, diffuseMat);//0.35, 0.35, 0.35);
      vpSetMaterial(vpc, VP_MATERIAL0, VP_SPECULAR, VP_BOTH_SIDES, 
                    specularMat, specularMat, specularMat);//0.39, 0.39, 0.39);
      vpSetMaterial(vpc, VP_MATERIAL0, VP_SHINYNESS, VP_BOTH_SIDES, 
                    shinyMat, 0.0, 0.0);//10.0,  0.0, 0.0);

      vpSetLight(vpc, VP_LIGHT0, VP_DIRECTION, 0.3, 0.3, 1.0);
      vpSetLight(vpc, VP_LIGHT0, VP_COLOR, 1.0, 1.0, 1.0);
      vpEnable(vpc, VP_LIGHT0, 1);
      
      vpSeti(vpc, VP_CONCAT_MODE, VP_CONCAT_LEFT);
    }
    cout << "----- make vp data time = " << ((clock()-time0)/1000000.0) << endl;
  }
  
}  // end MakeVPData()


// -------------------------------------------------------------------
void VolRender::MakeDefaultTransProperties() {
    classifyFields = 2;
    shadeFields = 2;
    nDenRampPts = 2;
    densityRampX.resize(nDenRampPts);
    densityRampY.resize(nDenRampPts);
    nGradRampPts = 2;
    gradientRampX.resize(nGradRampPts);
    gradientRampY.resize(nGradRampPts);
    densityRampX[0] = 0;    densityRampX[1] = 255;
    densityRampY[0] = 0.0;  densityRampY[1] = 1.0;

    gradientRampX[0] = 0;    gradientRampX[1] = 255;
    gradientRampY[0] = 0.0;  gradientRampY[1] = 1.0;

    minRayOpacity = 0.05;
    maxRayOpacity = 0.95;
}


// -------------------------------------------------------------------
void VolRender::SetTransferProperties() {
  BL_ASSERT(palettePtr != NULL);
  density_ramp = palettePtr->GetTransferArray();
  //density_ramp[palettePtr->BodyIndex()] = 0.08;
  density_ramp[palettePtr->BodyIndex()] = AVGlobals::GetBodyOpacity();
  vpSetClassifierTable(vpc, DENSITY_PARAM, densityField,
                       density_ramp.dataPtr(),
		       density_ramp.size() * sizeof(float));

  /*  if(classifyFields == 2) {
    vpRamp(gradient_ramp.dataPtr(), sizeof(float), gradientRampX.size(),
	   gradientRampX.dataPtr(), gradientRampY.dataPtr());
    vpSetClassifierTable(vpc, GRADIENT_PARAM, gradientField,
                         gradient_ramp.dataPtr(),
			 gradient_ramp.size() * sizeof(float));
  }*/

  vpSetd(vpc, VP_MIN_VOXEL_OPACITY, minRayOpacity);
  vpSetd(vpc, VP_MAX_RAY_OPACITY,   maxRayOpacity);
}


// -------------------------------------------------------------------
void VolRender::SetProperties() {
  // some init -- should be placed elsewhere
  // was previously read from vpramps.dat
  classifyFields = 1;
  shadeFields = 2;

  vpResult vpret = vpSetVoxelSize(vpc, BYTES_PER_VOXEL, voxelFields,
                        shadeFields, classifyFields);
  CheckVP(vpret, 14);
  if(lightingModel) {
    vpSetVoxelField(vpc,  normalField, normalSize, normalOffset,
                  maxShadeRampPts-1);
  } else {  // value model
    vpSetVoxelField(vpc,  normalField, normalSize, normalOffset,
                    paletteSize - 1);
  }
  vpSetVoxelField(vpc, densityField, densitySize, densityOffset,
                  densityMax);

  vpSetVoxelField(vpc, gradientField, gradientSize, gradientOffset,
                  gradientMax);
  SetTransferProperties();
}


// -------------------------------------------------------------------
void VolRender::SetLighting(Real ambient, Real diffuse, 
                            Real specular, Real shiny,
                            Real minRay, Real maxRay)
{
  ambientMat = ambient;
  diffuseMat = diffuse;
  specularMat = specular;
  shinyMat = shiny;
  minRayOpacity = minRay;
  maxRayOpacity = maxRay;
}
// -------------------------------------------------------------------
// -------------------------------------------------------------------

