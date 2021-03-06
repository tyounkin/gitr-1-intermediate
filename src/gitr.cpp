#include <iostream>
#include <chrono>
#include <cmath>
#include <fstream>
#include <stdlib.h>
#include <algorithm>
#include <random>
#include <iomanip>
#include <cstdlib>
#include <libconfig.h++>
#include <vector>
#include <netcdf>
#include "utils.h"
#include "boris.h"
#include "geometryCheck.h"
#include "ionize.h"
#include "recombine.h"
#include "crossFieldDiffusion.h"
#include "coulombCollisions.h"
#include "thermalForce.h"
#include "surfaceModel.h"
#include "interp2d.hpp"
#include "interpRateCoeff.hpp"
#include "Particles.h"
#include "Boundary.h"
#include "curandInitialize.h"
#include "spectroscopy.h"
#include "fieldLineTrace.h"
#include "history.h"
#include "io.hpp"
#include "hashGeom.h"
#include "hashGeomSheath.h"
#include "testRoutine.h"
#include "testRoutineCuda.h"
#include "boundaryInit.h"
#include "array.h"
#include "ompPrint.h"

#if USE_BOOST
    #include <boost/timer/timer.hpp>
    #include "boost/filesystem.hpp"
#endif

#ifdef __CUDACC__
    #include <curand.h>
    #include <curand_kernel.h>
#endif

#include <thrust/execution_policy.h>
#include <thrust/sequence.h>
#include <thrust/transform.h>
#include <thrust/functional.h>

using namespace std;
using namespace libconfig;

#if USE_BOOST
    using namespace boost::timer;
#endif

using namespace netCDF;
using namespace exceptions;
using namespace netCDF::exceptions;

int main()
{
  //Prepare config files for import
  Config cfg,cfg_geom;

  //Parse and read input file
  std::cout << "Open configuration file gitrInput.cfg " << std::endl;
  try
  {
    cfg.readFile("gitrInput.cfg");
  }
  catch(const FileIOException &fioex)
  {
    std::cerr << "I/O error while reading file gitrInput.cfg" << std::endl;
    return(EXIT_FAILURE);
  }
  catch(const ParseException &pex)
  {
    std::cerr << "Parse error at " << pex.getFile() << ":" << pex.getLine()
              << " - " << pex.getError() << std::endl;
    return(EXIT_FAILURE);
  }
  
  // Parse and read geometry file
  const char *geomFile; 
  if(cfg.lookupValue("geometry.fileString", geomFile))
  {
    try
    {
      cfg_geom.readFile(geomFile);
    }
    catch(const FileIOException &fioex)
    {
      std::cerr << "I/O error while reading GITR geometry file" << std::endl;
      return(EXIT_FAILURE);
    }
    catch(const ParseException &pex)
    {
      std::cerr << "Parse error at " << pex.getFile() << ":" << pex.getLine()
                << " - " << pex.getError() << std::endl;
      return(EXIT_FAILURE);
    }

  }
  
  std::cout << "Successfully staged input and geometry file " << std::endl;
  
  //check binary compatibility with input file
  #if CHECK_COMPATIBILITY>0
    std::cout << "Checking compatibility of compile flags with input file " 
              << std::endl;
   
    const char *flags0[] = {"flags.USE_CUDA","flags.USEMPI", 
              "flags.USE_BOOST","flags.USEIONIZATION",
              "flags.USERECOMBINATION","flags.USEPERPDIFFUSION",
              "flags.USECOULOMBCOLLISIONS",
              "flags.USETHERMALFORCE","flags.USESURFACEMODEL",
              "flags.USESHEATHEFIELD",
              "flags.USEPRESHEATHEFIELD","flags.BFIELD_INTERP",
              "flags.LC_INTERP","flags.GENERATE_LC", "flags.EFIELD_INTERP",
              "flags.PRESHEATH_INTERP","flags.DENSITY_INTERP",
              "flags.TEMP_INTERP",
              "flags.FLOWV_INTERP","flags.GRADT_INTERP",
              "flags.ODEINT","flags.FIXEDSEEDS",
              "flags.PARTICLESEEDS","flags.GEOM_TRACE","flags.GEOM_HASH",
              "flags.GEOM_HASH_SHEATH","flags.PARTICLE_TRACKS",
              "flags.PARTICLE_SOURCE",
              "flags.SPECTROSCOPY","flags.USE3DTETGEOM","flags.USECYLSYMM"};
    int flagValues[] =  {USE_CUDA, USEMPI, USE_BOOST,USEIONIZATION,
           USERECOMBINATION,USEPERPDIFFUSION,USECOULOMBCOLLISIONS,
           USETHERMALFORCE,USESURFACEMODEL,USESHEATHEFIELD,
           USEPRESHEATHEFIELD,BFIELD_INTERP,LC_INTERP, GENERATE_LC,
           EFIELD_INTERP,
           PRESHEATH_INTERP,DENSITY_INTERP,TEMP_INTERP,
           FLOWV_INTERP,GRADT_INTERP,ODEINT,FIXEDSEEDS,
           PARTICLESEEDS,GEOM_TRACE,GEOM_HASH,
           GEOM_HASH_SHEATH,PARTICLE_TRACKS,PARTICLE_SOURCE,
           SPECTROSCOPY,USE3DTETGEOM,USECYLSYMM};
    int check1;
    for (int i=0; i<sizeof(flagValues)/sizeof(int); i++)
    {
      if(cfg.lookupValue(flags0[i], check1))
      {  
        if (flagValues[i] != check1)
        { std::cout << "incompatibility in " << flags0[i]
                  << " between input file and binary" << std::endl;
          exit(0);
        }
        else
        {
          std::cout << flags0[i] <<" = " << check1<< std::endl;
        }
      }
      else
      {
        std::cout << flags0[i] <<" was not found" << std::endl;
      }
    }
  #endif
  
  // show memory usage of GPU
  #if USE_CUDA 
    size_t free_byte ;
    size_t total_byte ;
    cudaError_t    cuda_status = cudaMemGetInfo( &free_byte, &total_byte ) ;
  
    if(cudaSuccess != cuda_status )
    {
  
       printf("Error: cudaMemGetInfo fails, %s \n", cudaGetErrorString(cuda_status) );
       exit(1);
    }
  
    double free_db = (double)free_byte ;
    double total_db = (double)total_byte ;
    double used_db = total_db - free_db ;
    
    printf("GPU memory usage: used = %f, free = %f MB, total = %f MB\n",
      used_db/1024.0/1024.0, free_db/1024.0/1024.0, total_db/1024.0/1024.0); 
  #endif
  
  // Background species info
  float background_Z,background_amu;
  getVariable(cfg,"backgroundPlasmaProfiles.Z",background_Z);
  getVariable(cfg,"backgroundPlasmaProfiles.amu",background_amu);

  //Bfield initialization
  int nR_Bfield = 1;
  int nY_Bfield = 1;
  int nZ_Bfield = 1;
  int n_Bfield = 1;
  std::string bfieldCfg = "backgroundPlasmaProfiles.Bfield.";
  #if BFIELD_INTERP > 0
    std::string bfieldFile;
    getVariable(cfg,bfieldCfg+"fileString",bfieldFile);
    nR_Bfield = getDimFromFile(cfg,bfieldFile,bfieldCfg,"gridNrString");
  #endif
  #if BFIELD_INTERP > 1
    nZ_Bfield = getDimFromFile(cfg,bfieldFile,bfieldCfg,"gridNzString");
  #endif
  #if BFIELD_INTERP > 2
    nY_Bfield = getDimFromFile(cfg,bfieldFile,bfieldCfg,"gridNyString");
  #endif
  sim::Array<float> bfieldGridr(nR_Bfield),bfieldGridy(nY_Bfield),bfieldGridz(nZ_Bfield);
  n_Bfield = nR_Bfield*nY_Bfield*nZ_Bfield;
  sim::Array<float> br(n_Bfield),by(n_Bfield),bz(n_Bfield);
  #if BFIELD_INTERP == 0
    getVariable(cfg,bfieldCfg+"r",br[0]);
    getVariable(cfg,bfieldCfg+"y",by[0]);
    getVariable(cfg,bfieldCfg+"z",bz[0]);
  #else
    getVarFromFile(cfg,bfieldFile,bfieldCfg,"gridRString",bfieldGridr);
    #if BFIELD_INTERP > 1
      getVarFromFile(cfg,bfieldFile,bfieldCfg,"gridZString",bfieldGridz);
    #endif
    #if BFIELD_INTERP > 2
      getVarFromFile(cfg,bfieldFile,bfieldCfg,"gridYString",bfieldGridy);
    #endif

    getVarFromFile(cfg,bfieldFile,bfieldCfg,"rString",br);
    getVarFromFile(cfg,bfieldFile,bfieldCfg,"yString",by);
    getVarFromFile(cfg,bfieldFile,bfieldCfg,"zString",bz);
  #endif  
  std::cout << "Finished Bfield import" << std::endl; 

  std::string profiles_folder = "profiles";  
  
  //Geometry Definition
  Setting& geom = cfg_geom.lookup("geom");
  int nLines = geom["x1"].getLength();
  //int nMaterials = geom["nMaterials"];
  std::cout << "Number of Geometric Objects To Load: " << nLines << std::endl;
  sim::Array<Boundary> boundaries(nLines+1);
  importGeometry(cfg_geom, boundaries);

  std::cout << "Starting Boundary Init..." << std::endl;
  float biasPotential = 0.0;
  
  #if BIASED_SURFACE > 0
    getVariable(cfg,"backgroundPlasmaProfiles.biasPotential",biasPotential);
  #endif
  
  int nR_closeGeom = 1;
  int nY_closeGeom = 1;
  int nZ_closeGeom = 1;
  int n_closeGeomElements = 1;
  int nGeomHash = 1;
  std::string geomHashCfg = "geometry_hash.";
  #if GEOM_HASH == 1
    getVariable(cfg,geomHashCfg+"nR_closeGeom",nR_closeGeom);
    getVariable(cfg,geomHashCfg+"nZ_closeGeom",nZ_closeGeom);
    getVariable(cfg,geomHashCfg+"n_closeGeomElements",n_closeGeomElements);
    nGeomHash = nR_closeGeom*nZ_closeGeom*n_closeGeomElements;
    #if USE3DTETGEOM > 0
      getVariable(cfg,geomHashCfg+"nY_closeGeom",nY_closeGeom);
      nGeomHash = nY_closeGeom*nGeomHash;
    #endif
  #endif

  #if GEOM_HASH > 1
    std::string hashFile;
    getVariable(cfg,geomHashCfg+"fileString",hashFile);
    nR_closeGeom = getDimFromFile(cfg,hashFile,geomHashCfg,"gridNrString");
    nZ_closeGeom = getDimFromFile(cfg,hashFile,geomHashCfg,"gridNzString");
    n_closeGeomElements = getDimFromFile(cfg,hashFile,geomHashCfg,"nearestNelementsString");
    nGeomHash = nR_closeGeom*nZ_closeGeom*n_closeGeomElements;
    #if USE3DTETGEOM > 0
      nY_closeGeom = getDimFromFile(cfg,hashFile,geomHashCfg,"gridNyString");
      nGeomHash = nY_closeGeom*nGeomHash;
    #else
    #endif
  #endif
  sim::Array<float> closeGeomGridr(nR_closeGeom), closeGeomGridy(nY_closeGeom),
      closeGeomGridz(nZ_closeGeom);
  sim::Array<int> closeGeom(nGeomHash);
  #if GEOM_HASH == 1
    float hashX0,hashX1,hashY0,hashY1,hashZ0,hashZ1;
    getVariable(cfg,geomHashCfg+"hashX0",hashX0);
    getVariable(cfg,geomHashCfg+"hashX1",hashX1);
    getVariable(cfg,geomHashCfg+"hashZ0",hashZ0);
    getVariable(cfg,geomHashCfg+"hashZ1",hashZ1);
    #if USE3DTETGEOM > 0
      getVariable(cfg,geomHashCfg+"hashY0",hashY0);
      getVariable(cfg,geomHashCfg+"hashY1",hashY1);
    #endif
    
    for(int i=0; i<nR_closeGeom; i++)
    {  closeGeomGridr[i] = (hashX1 - hashX0)*i/(nR_closeGeom - 1)+ hashX0;}
    for(int j=0; j<nY_closeGeom; j++)
    {  closeGeomGridy[j] = (hashY1 - hashY0)*j/(nY_closeGeom - 1)+ hashY0;}
    for(int k=0; k<nZ_closeGeom; k++)
    {  closeGeomGridz[k] = (hashZ1 - hashZ0)*k/(nZ_closeGeom - 1)+ hashZ0;}
  
    thrust::counting_iterator<std::size_t> lines0(0);  
    thrust::counting_iterator<std::size_t> lines1(nR_closeGeom*nY_closeGeom);
    sim::Array<float> minDist1(nGeomHash,1e6);

    for(int i=0; i<nZ_closeGeom; i++)
    {
       thrust::for_each(thrust::device, lines0,lines1,
                        hashGeom(i,nLines, boundaries.data(), 
                        closeGeomGridr.data(), closeGeomGridy.data(),
                        closeGeomGridz.data(),
                        n_closeGeomElements, minDist1.data(), closeGeom.data(),
                        nR_closeGeom,nY_closeGeom,nZ_closeGeom));
       #if USE_CUDA
         cudaDeviceSynchronize();
       #endif
    }
    #if USE_CUDA
      cudaDeviceSynchronize();
    #endif
    std::vector<int> geomHashDims(4);
    geomHashDims[0] = nR_closeGeom;
    geomHashDims[1] = nY_closeGeom;
    geomHashDims[2] = nZ_closeGeom;
    geomHashDims[3] = n_closeGeomElements;
    std::vector<std::string> geomHashDimNames(4);
    geomHashDimNames[0] = "nR";
    geomHashDimNames[1] = "nY";
    geomHashDimNames[2] = "nZ";
    geomHashDimNames[3] = "n_";
    std::vector<std::string> hashGridNames(2);
    hashGridNames[0] = "gridR";
    hashGridNames[1] = "gridZ";
    std::vector<int> hashGridMapDim(2);
    hashGridMapDim[0] = 0;
    hashGridMapDim[1] = 2;
    std::vector<std::vector<float>> hashGrids(2);
    hashGrids[0].assign(&closeGeomGridr[0], &closeGeomGridr[0]+nR_closeGeom);
    hashGrids[1].assign(&closeGeomGridz[0], &closeGeomGridz[0]+nZ_closeGeom);
    std::vector<float*> hashGridPointers(2);
    hashGridPointers[0] = &closeGeomGridr[0];
    hashGridPointers[1] = &closeGeomGridz[0];
    std::vector<std::string> intVarNames(1);
    intVarNames[0] = "hash";
    std::vector<vector<int>> intVarDimMap(1);
    intVarDimMap[0].push_back(0);
    intVarDimMap[0].push_back(2);
    intVarDimMap[0].push_back(3);
    std::vector<int*> intVarPointers(1);
    intVarPointers[0] = &closeGeom[0];
    std::string hashOutfile= "GITRgeomHash";
    ncdfIO(1,hashOutfile,geomHashDimNames,geomHashDims,
            hashGridNames,hashGridMapDim,hashGridPointers,
            intVarNames,intVarDimMap,intVarPointers);
  #elif GEOM_HASH > 1
    getVarFromFile(cfg,hashFile,geomHashCfg,"gridRString",closeGeomGridr);
    getVarFromFile(cfg,hashFile,geomHashCfg,"gridZString",closeGeomGridz);
    #if USE3DTETGEOM >0
      getVarFromFile(cfg,hashFile,geomHashCfg,"gridYString",closeGeomGridy);
    #endif
      getVarFromFile(cfg,hashFile,geomHashCfg,"closeGeomString",closeGeom);
  #endif
              
  int nR_closeGeom_sheath = 1;
  int nY_closeGeom_sheath = 1;
  int nZ_closeGeom_sheath = 1;
  int n_closeGeomElements_sheath = 1;
  int nGeomHash_sheath = 1;
  std::string geomHashSheathCfg= "geometry_sheath.";  
  #if GEOM_HASH_SHEATH == 1
    getVariable(cfg,geomHashSheathCfg+"nR_closeGeom",nR_closeGeom_sheath);
    getVariable(cfg,geomHashSheathCfg+"nZ_closeGeom",nZ_closeGeom_sheath);
    getVariable(cfg,geomHashSheathCfg+"n_closeGeomElements",n_closeGeomElements_sheath);
    nGeomHash_sheath = nR_closeGeom_sheath*nZ_closeGeom_sheath*n_closeGeomElements_sheath;
    #if USE3DTETGEOM > 0
      getVariable(cfg,geomHashSheathCfg+"nY_closeGeom",nY_closeGeom_sheath);
      nGeomHash_sheath = nY_closeGeom_sheath*nGeomHash_sheath;
    #endif
  #endif

  #if GEOM_HASH_SHEATH > 1
    std::string hashFile_sheath;
    getVariable(cfg,geomHashSheathCfg+"fileString",hashFile_sheath);
    nR_closeGeom_sheath = getDimFromFile(cfg,hashFile_sheath,geomHashSheathCfg,"gridNrString");
    nZ_closeGeom_sheath = getDimFromFile(cfg,hashFile_sheath,geomHashSheathCfg,"gridNzString");
    n_closeGeomElements_sheath = getDimFromFile(cfg,hashFile_sheath,geomHashSheathCfg,"nearestNelementsString");
    nGeomHash_sheath = nR_closeGeom_sheath*nZ_closeGeom_sheath*n_closeGeomElements_sheath;
    #if USE3DTETGEOM > 0
      nY_closeGeom_sheath = getDimFromFile(cfg,hashFile_sheath,geomHashSheathCfg,"gridNyString");
      nGeomHash_sheath = nY_closeGeom_sheath*nGeomHash_sheath;
    #else
    #endif
  #endif
  sim::Array<float> closeGeomGridr_sheath(nR_closeGeom_sheath), 
                    closeGeomGridy_sheath(nY_closeGeom_sheath),
                    closeGeomGridz_sheath(nZ_closeGeom_sheath);
  sim::Array<int> closeGeom_sheath(nGeomHash_sheath);
  #if GEOM_HASH_SHEATH  ==1
    float hashX0_s,hashX1_s,hashY0_s,hashY1_s,hashZ0_s,hashZ1_s;
    getVariable(cfg,geomHashSheathCfg+"hashX0",hashX0_s);
    getVariable(cfg,geomHashSheathCfg+"hashX1",hashX1_s);
    getVariable(cfg,geomHashSheathCfg+"hashZ0",hashZ0_s);
    getVariable(cfg,geomHashSheathCfg+"hashZ1",hashZ1_s);
    #if USE3DTETGEOM > 0
      getVariable(cfg,geomHashSheathCfg+"hashY0",hashY0_s);
      getVariable(cfg,geomHashSheathCfg+"hashY1",hashY1_s);
    #endif
    
    for(int i=0; i<nR_closeGeom_sheath; i++)
    {  closeGeomGridr_sheath[i] = (hashX1_s - hashX0_s)*i/(nR_closeGeom_sheath - 1)+ hashX0_s;}
    for(int j=0; j<nY_closeGeom_sheath; j++)
    {  closeGeomGridy_sheath[j] = (hashY1_s - hashY0_s)*j/(nY_closeGeom_sheath - 1)+ hashY0_s;}
    for(int k=0; k<nZ_closeGeom_sheath; k++)
    {  closeGeomGridz_sheath[k] = (hashZ1_s - hashZ0_s)*k/(nZ_closeGeom_sheath - 1)+ hashZ0_s;}
      
    thrust::counting_iterator<std::size_t> lines0_s(0);  
    thrust::counting_iterator<std::size_t> lines1_s(nR_closeGeom_sheath*nY_closeGeom_sheath);
    sim::Array<float> minDist1_s(nGeomHash_sheath,1e6);
    
    for(int i=0; i<nZ_closeGeom_sheath; i++)
      {
              thrust::for_each(thrust::device, lines0_s,lines1_s,
                               hashGeom_sheath(i,nLines, boundaries.data(), closeGeomGridr_sheath.data(),
                               closeGeomGridy_sheath.data(),closeGeomGridz_sheath.data(),
                               n_closeGeomElements_sheath, minDist1_s.data(), closeGeom_sheath.data(),
                               nR_closeGeom_sheath,nY_closeGeom_sheath,nZ_closeGeom_sheath));
              cudaDeviceSynchronize();
      }
              cudaDeviceSynchronize();

    std::vector<int> geomHashDims_s(4);
    geomHashDims_s[0] = nR_closeGeom_sheath;
    geomHashDims_s[1] = nY_closeGeom_sheath;
    geomHashDims_s[2] = nZ_closeGeom_sheath;
    geomHashDims_s[3] = n_closeGeomElements_sheath;
    std::vector<std::string> geomHashDimNames_s(4);
    geomHashDimNames_s[0] = "nR";
    geomHashDimNames_s[1] = "nY";
    geomHashDimNames_s[2] = "nZ";
    geomHashDimNames_s[3] = "n_";
    std::vector<std::string> hashGridNames_s(2);
    hashGridNames_s[0] = "gridR";
    hashGridNames_s[1] = "gridZ";
    std::vector<int> hashGridMapDim_s(2);
    hashGridMapDim_s[0] = 0;
    hashGridMapDim_s[1] = 2;
    std::vector<float*> hashGridPointers_s(2);
    hashGridPointers_s[0] = &closeGeomGridr_sheath[0];
    hashGridPointers_s[1] = &closeGeomGridz_sheath[0];
    std::vector<std::string> intVarNames_s(1);
    intVarNames_s[0] = "hash_sheath";
    std::vector<vector<int>> intVarDimMap_s(1);
    intVarDimMap_s[0].push_back(0);
    intVarDimMap_s[0].push_back(2);
    intVarDimMap_s[0].push_back(3);
    std::vector<int*> intVarPointers_s(1);
    intVarPointers_s[0] = &closeGeom_sheath[0];
    std::string hashOutfile_s= "GITRgeomHash_sheath";
    ncdfIO(1,hashOutfile_s,geomHashDimNames_s,geomHashDims_s,
            hashGridNames_s,hashGridMapDim_s,hashGridPointers_s,
            intVarNames_s,intVarDimMap_s,intVarPointers_s);
  #elif GEOM_HASH_SHEATH > 1
    getVarFromFile(cfg,hashFile_sheath,geomHashSheathCfg,"gridRString",closeGeomGridr_sheath);
    getVarFromFile(cfg,hashFile_sheath,geomHashSheathCfg,"gridZString",closeGeomGridz_sheath);
    #if USE3DTETGEOM >0
      getVarFromFile(cfg,hashFile_sheath,geomHashSheathCfg,"gridYString",closeGeomGridy_sheath);
    #endif
      getVarFromFile(cfg,hashFile_sheath,geomHashSheathCfg,"closeGeomString",closeGeom_sheath);
  #endif

  int nR_Lc = 1;
  int nY_Lc = 1;
  int nZ_Lc = 1;
  int nTracers = 1;
  std::string connLengthCfg= "connectionLength.";  
  std::string lcFile;
  getVariable(cfg,connLengthCfg+"fileString",lcFile);
  #if GENERATE_LC == 1
    getVariable(cfg,connLengthCfg+"nX",nR_Lc);
    getVariable(cfg,connLengthCfg+"nY",nY_Lc);
    getVariable(cfg,connLengthCfg+"nZ",nZ_Lc);
    float r0_Lc, r1_Lc, y0_Lc,y1_Lc,z0_Lc,z1_Lc, dr;
    int nTraceSteps;
    getVariable(cfg,connLengthCfg+"netx0",r0_Lc);
    getVariable(cfg,connLengthCfg+"netx1",r1_Lc);
    getVariable(cfg,connLengthCfg+"nety0",y0_Lc);
    getVariable(cfg,connLengthCfg+"nety1",y1_Lc);
    getVariable(cfg,connLengthCfg+"netz0",z0_Lc);
    getVariable(cfg,connLengthCfg+"netz1",z1_Lc);
    getVariable(cfg,connLengthCfg+"nTraceSteps",nTraceSteps);
    getVariable(cfg,connLengthCfg+"dr",dr);
  #endif 
  #if GENERATE_LC > 1
    nR_Lc = getDimFromFile(cfg,lcFile,connLengthCfg,"gridNrString");
    nY_Lc = getDimFromFile(cfg,lcFile,connLengthCfg,"gridNyString");
    nZ_Lc = getDimFromFile(cfg,lcFile,connLengthCfg,"gridNzString");
  #endif
  
  #if USE3DTETGEOM > 0
    nTracers = nR_Lc*nY_Lc*nZ_Lc;
  #else
    nTracers = nR_Lc*nZ_Lc;
  #endif
  
  sim::Array<float> Lc(nTracers),s(nTracers);
  sim::Array<float> gridRLc(nR_Lc),gridYLc(nY_Lc),gridZLc(nZ_Lc);
  sim::Array<int> noIntersectionNodes(nTracers);
  #if GENERATE_LC ==1
    if( !boost::filesystem::exists( lcFile ) )
    {
       std::cout << "No pre-existing connection length file found" << std::endl;    
      #if USE3DTETGEOM > 0
        float dy_Lc = (y1_Lc-y0_Lc)/(nY_Lc-1);
        for(int j=0;j<nY_Lc; j++)
        {
         gridYLc[j] = y0_Lc + j*dy_Lc;
        }
      #endif
      float dr_Lc = (r1_Lc-r0_Lc)/(nR_Lc-1);
      for(int i=0;i<nR_Lc; i++)
      {
       gridRLc[i] = r0_Lc + i*dr_Lc;
      }

      float dz_Lc = (z1_Lc-z0_Lc)/(nZ_Lc-1);
      for(int j=0;j<nZ_Lc; j++)
      {
       gridZLc[j] = z0_Lc + j*dz_Lc;
      }
      std::cout << "Creating tracer particles" << std::endl;
      thrust::counting_iterator<std::size_t> lcBegin(0);  
      thrust::counting_iterator<std::size_t> lcEnd(nTracers);
      auto forwardTracerParticles = new Particles(nTracers);
      auto backwardTracerParticles = new Particles(nTracers);
      int addIndex = 0;
      std::cout << "Initializing tracer particles" << std::endl;
   
      for(int i=0;i<nR_Lc; i++)
      {
        for(int j=0;j<nY_Lc;j++)
        {
           for (int k=0;k<nZ_Lc;k++)
           {
              #if USE3DTETGEOM > 0
                addIndex = i + j*nR_Lc + k*nR_Lc*nY_Lc;
              #else
                addIndex = i+k*nR_Lc;
              #endif
              forwardTracerParticles->setParticle(addIndex,gridRLc[i], gridYLc[j], gridZLc[k], 
                                                  0.0, 0.0, 0.0, 0, 0.0, 0.0);
              backwardTracerParticles->setParticle(addIndex,gridRLc[i], gridYLc[j], gridZLc[k], 
                                                   0.0, 0.0, 0.0, 0, 0.0, 0.0);
           }
        }
      }
      
                              
      typedef std::chrono::high_resolution_clock Time_trace;
      typedef std::chrono::duration<float> fsec_trace;
      auto start_clock_trace = Time_trace::now();
      std::cout << "Starting trace loop" << std::endl;
      std::cout << "nTraceSteps"<< nTraceSteps << " dr "<< dr  << std::endl;
      for (int ii=0;ii<nTraceSteps; ii++)
      {
        #if USE_CUDA 
          cudaDeviceSynchronize();
        #endif
        thrust::for_each(thrust::device, lcBegin,lcEnd,
                 field_line_trace(1.0,forwardTracerParticles,dr,boundaries.data(), nLines,
                     nR_Lc,nZ_Lc,gridRLc.data(),gridZLc.data(),Lc.data(),
                     nR_Bfield,nZ_Bfield, bfieldGridr.data(),&bfieldGridz.front(),
                     &br.front(),&bz.front(),&by.front()));
        
        thrust::for_each(thrust::device, lcBegin,lcEnd,
                 field_line_trace(-1.0,backwardTracerParticles,dr,boundaries.data(), nLines,
                     nR_Lc,nZ_Lc,gridRLc.data(),gridZLc.data(),Lc.data(),
                     nR_Bfield,nZ_Bfield, bfieldGridr.data(),&bfieldGridz.front(),
                     &br.front(),&bz.front(),&by.front()));
            
        thrust::for_each(thrust::device, lcBegin,lcEnd,
                  geometry_check(forwardTracerParticles,nLines,&boundaries[0],dr,ii,
                      nR_closeGeom,nY_closeGeom,nZ_closeGeom,n_closeGeomElements,
                      &closeGeomGridr.front(),&closeGeomGridy.front(),&closeGeomGridz.front(),
                      &closeGeom.front()) );
        
        thrust::for_each(thrust::device, lcBegin,lcEnd,
                  geometry_check(backwardTracerParticles,nLines,&boundaries[0],dr,ii,
                      nR_closeGeom,nY_closeGeom,nZ_closeGeom,n_closeGeomElements,
                      &closeGeomGridr.front(),&closeGeomGridy.front(),&closeGeomGridz.front(),
                      &closeGeom.front()) );
      }
      auto finish_clock_trace = Time_trace::now();
      fsec_trace fstrace = finish_clock_trace - start_clock_trace;
      printf("Time taken          is %6.3f (secs) \n", fstrace.count());
      printf("Time taken per step is %6.3f (secs) \n", fstrace.count() / (float) nTraceSteps);
      #if USE_CUDA 
         cudaDeviceSynchronize();
      #endif
      addIndex = 0;
      float forwardDist = 0.0;
      float backwardDist = 0.0;
      for(int i=0;i<nR_Lc; i++)
      {
        for(int j=0;j<nY_Lc;j++)
        {
          for(int k=0;k<nZ_Lc;k++)
          {
            
             #if USE3DTETGEOM > 0
               addIndex = i + j*nR_Lc + k*nR_Lc*nY_Lc;
             #else
                   addIndex = i+k*nR_Lc;
             #endif
             if(forwardTracerParticles->hitWall[addIndex] > 0)
             {
                forwardDist = forwardTracerParticles->distanceTraveled[addIndex];
             }
             else
             { forwardDist = 0.0;}

             if(backwardTracerParticles->hitWall[addIndex] > 0)
             {
                backwardDist = backwardTracerParticles->distanceTraveled[addIndex];
             }
             else backwardDist = 0.0;
             
             Lc[addIndex] = forwardDist + backwardDist;
             
             if(forwardTracerParticles->distanceTraveled[addIndex] > 
                     backwardTracerParticles->distanceTraveled[addIndex])
             {
               s[addIndex] = -(0.5*Lc[addIndex]-backwardTracerParticles->distanceTraveled[addIndex]);
             }
             else
             {
               s[addIndex] = (0.5*Lc[addIndex]-forwardTracerParticles->distanceTraveled[addIndex]);
             }
             if(forwardTracerParticles->hitWall[addIndex] + backwardTracerParticles->hitWall[addIndex]<4.0)
             {
               noIntersectionNodes[addIndex] = 1;    
             }
          }
        }
      }

      NcFile ncFileLC("LcS.nc", NcFile::replace);
      NcDim nc_nTracers = ncFileLC.addDim("nTracers",nTracers);
      NcDim nc_nRLc = ncFileLC.addDim("nR",nR_Lc);
      NcDim nc_nYLc = ncFileLC.addDim("nY",nY_Lc);
      NcDim nc_nZLc = ncFileLC.addDim("nZ",nZ_Lc);
      
      NcVar nc_Lc = ncFileLC.addVar("Lc",ncDouble,nc_nTracers);
      NcVar nc_s = ncFileLC.addVar("s",ncDouble,nc_nTracers);
      NcVar nc_nI = ncFileLC.addVar("noIntersection",ncDouble,nc_nTracers);
      NcVar nc_gridRLc = ncFileLC.addVar("gridR",ncDouble,nc_nRLc);
      NcVar nc_gridYLc = ncFileLC.addVar("gridY",ncDouble,nc_nYLc);
      NcVar nc_gridZLc = ncFileLC.addVar("gridZ",ncDouble,nc_nZLc);
      
      nc_Lc.putVar(&Lc[0]);
      nc_s.putVar(&s[0]);
      nc_nI.putVar(&noIntersectionNodes[0]);
      nc_gridRLc.putVar(&gridRLc[0]);
      nc_gridYLc.putVar(&gridYLc[0]);
      nc_gridZLc.putVar(&gridZLc[0]);
      #if USE_CUDA 
             cudaDeviceSynchronize();
      #endif
   }         
  #endif    

  #if GENERATE_LC > 1
    std::cout << "Importing pre-existing connection length file" << std::endl;
    getVariable(cfg,connLengthCfg+"fileString",r0_Lc);
    getVarFromFile(cfg,lcFile,connLengthCfg,"gridRString",gridRLc);
    getVarFromFile(cfg,lcFile,connLengthCfg,"gridYString",gridYLc);
    getVarFromFile(cfg,lcFile,connLengthCfg,"gridZString",gridZLc);
    getVarFromFile(cfg,lcFile,connLengthCfg,"LcString",Lc);
    getVarFromFile(cfg,lcFile,connLengthCfg,"SString",s);
  #endif
  
  //Background Plasma Temperature Initialization    
  #if TEMP_INTERP == 0
    int nR_Temp = 1;
    int nZ_Temp = 1;
    sim::Array<float> TempGridr(nR_Temp), TempGridz(nZ_Temp);
    sim::Array<float> ti(nR_Temp*nZ_Temp), te(nR_Temp*nZ_Temp);
    if(cfg.lookupValue("backgroundPlasmaProfiles.Temperature.ti", ti[0]) && 
       cfg.lookupValue("backgroundPlasmaProfiles.Temperature.te", te[0]))
    {std::cout << "Ti and Te = " << ti[0] << " " << te[0] << std::endl;}
    else
    {std::cout << "ERROR: Failed importing constant temperatures" << std:: endl;}
  #elif TEMP_INTERP == 2
    int nR_Temp;
    int nZ_Temp;
    const char *tempFile, *tempNr, *tempNz, *tempGridR, *tempGridZ, *teChar, *tiChar;
    if(cfg.lookupValue("backgroundPlasmaProfiles.Temperature.fileString", tempFile) &&
       cfg.lookupValue("backgroundPlasmaProfiles.Temperature.gridNrString", tempNr) &&
       cfg.lookupValue("backgroundPlasmaProfiles.Temperature.gridNzString", tempNz) &&
       cfg.lookupValue("backgroundPlasmaProfiles.Temperature.gridRString",tempGridR) &&
       cfg.lookupValue("backgroundPlasmaProfiles.Temperature.gridZString",tempGridZ) &&
       cfg.lookupValue("backgroundPlasmaProfiles.Temperature.IonTempString", tiChar) &&
       cfg.lookupValue("backgroundPlasmaProfiles.Temperature.ElectronTempString", teChar))
    { std::cout << "Temperature file: " << tempFile << std::endl;}
    else
    { std::cout << "ERROR: Failed in acquiring Temperature file data from input file " << std::endl;} 
    std::cout << "read netcdf file" << std::endl;
    int t1 = read_profileNsChar(tempFile,tempNr,tempNz,nR_Temp,nZ_Temp);
    std::cout << tempNr << nR_Temp << std::endl; 
    sim::Array<float> TempGridr(nR_Temp), TempGridz(nZ_Temp);
    sim::Array<float> ti(nR_Temp*nZ_Temp), te(nR_Temp*nZ_Temp);
    
    int t2 = read_profile1d(tempFile,tempGridR, TempGridr);
    
    int t3 = read_profile1d(tempFile,tempGridZ, TempGridz);
    
    int t4 = read_profile2d(tempFile,tiChar, ti);
    
    int t5 = read_profile2d(tempFile,teChar, te);
  #endif
  std::string outnameTgridR = "tGridR.m";
  std::string outnameTgridZ = "tGridZ.m";
  std::string outnameTi = "ti.m";
  std::string outnameTe = "te.m";
  OUTPUT1d(profiles_folder,outnameTgridR, nR_Temp, &TempGridr.front());
  OUTPUT1d(profiles_folder,outnameTgridZ, nZ_Temp, &TempGridz.front());
  OUTPUT2d(profiles_folder,outnameTi, nR_Temp, nZ_Temp, &ti.front());
  OUTPUT2d(profiles_folder,outnameTe, nR_Temp, nZ_Temp, &te.front());

  float testVec = 0.0;
  testVec = interp2dCombined(0.07,0.0,0.0,nR_Temp,
                    nZ_Temp,TempGridr.data(),TempGridz.data(),ti.data());
  std::cout << "Finished Temperature import "<< testVec << std::endl; 
  
  //Background Plasma Density Initialization
  #if DENSITY_INTERP == 0
    int nR_Dens = 1;
    int nZ_Dens = 1;
    sim::Array<float> DensGridr(nR_Dens), DensGridz(nZ_Dens);
    sim::Array<float> ni(nR_Dens*nZ_Dens), ne(nR_Dens*nZ_Dens);
    if(cfg.lookupValue("backgroundPlasmaProfiles.Density.ni", ni[0]) && 
       cfg.lookupValue("backgroundPlasmaProfiles.Density.ne", ne[0]))
    {std::cout << "Ni and Ne = " << ni[0] << " " << ne[0] << std::endl;}
    else
    {std::cout << "ERROR: Failed importing constant densities" << std:: endl;}
  #elif DENSITY_INTERP == 2
    int nR_Dens;
    int nZ_Dens;
    
    const char *densFile, *densNr, *densNz, *densGridR, *densGridZ, *neChar, *niChar;
    if(cfg.lookupValue("backgroundPlasmaProfiles.Density.fileString", densFile) &&
       cfg.lookupValue("backgroundPlasmaProfiles.Density.gridNrString", densNr) &&
       cfg.lookupValue("backgroundPlasmaProfiles.Density.gridNzString", densNz) &&
       cfg.lookupValue("backgroundPlasmaProfiles.Density.gridRString",densGridR) &&
       cfg.lookupValue("backgroundPlasmaProfiles.Density.gridZString",densGridZ) &&
       cfg.lookupValue("backgroundPlasmaProfiles.Density.IonDensityString", niChar) &&
       cfg.lookupValue("backgroundPlasmaProfiles.Density.ElectronDensityString", neChar))
    { std::cout << "Density file: " << densFile << std::endl;}
    else
    { std::cout << "ERROR: Failed in acquiring density file data from input file " << std::endl;} 
    int n1 = read_profileNs(densFile,densNr,densNz,nR_Dens,nZ_Dens);
    
    sim::Array<float> DensGridr(nR_Dens), DensGridz(nZ_Dens);
    sim::Array<float> ni(nR_Dens*nZ_Dens), ne(nR_Dens*nZ_Dens);
    
    int n2 = read_profile1d(densFile,densGridR, DensGridr);
    
    int n3 = read_profile1d(densFile,densGridZ, DensGridz);
    
    int n4 = read_profile2d(densFile,niChar, ni);
    
    int n5 = read_profile2d(densFile,neChar, ne);
  #endif
  
  std::string outnameNgridR = "nGridR.m";
  std::string outnameNgridZ = "nGridZ.m";
  std::string outnameNi = "ni.m";
  std::string outnameNe = "ne.m";
  OUTPUT1d(profiles_folder,outnameNgridR, nR_Dens, &DensGridr.front());
  OUTPUT1d(profiles_folder,outnameNgridZ, nZ_Dens, &DensGridz.front());
  OUTPUT2d(profiles_folder,outnameNi, nR_Dens, nZ_Dens, &ni.front());
  OUTPUT2d(profiles_folder,outnameNe, nR_Dens, nZ_Dens, &ne.front());

  std::cout << "Finished density import "<< ne[0] << std::endl; 
  //Background Plasma flow velocity initialization    
  #if FLOWV_INTERP == 0
    int nR_flowV = 1;
    int nZ_flowV = 1;
    sim::Array<float> flowVGridr(nR_flowV), flowVGridz(nZ_flowV);
    sim::Array<float> flowVr(nR_flowV*nZ_flowV), flowVz(nR_flowV*nZ_flowV),
                        flowVt(nR_flowV*nZ_flowV);
    if(cfg.lookupValue("backgroundPlasmaProfiles.FlowVelocity.flowVr", flowVr[0]) && 
       cfg.lookupValue("backgroundPlasmaProfiles.FlowVelocity.flowVz", flowVz[0]) &&
       cfg.lookupValue("backgroundPlasmaProfiles.FlowVelocity.flowVt", flowVt[0]))
    {std::cout << "Flow velocity vector = " << flowVr[0] << " " << 
                  flowVt[0]<< " " << flowVz[0] << std::endl;}
    else
    {std::cout << "ERROR: Failed importing constant flow velocity" << std:: endl;}
  #elif FLOWV_INTERP == 1
    int nR_flowV=nR_Lc;
    int nZ_flowV=nZ_Lc;
    sim::Array<float> flowVGridr(nR_flowV);//=gridRLc;
    sim::Array<float> flowVGridz(nZ_flowV);//=gridZLc;
    std::cout << "nR_flowV " << nR_flowV << " " << nZ_flowV << std::endl;
    std::cout << " !!! gridRLc " << gridRLc[0] << std::endl;
    std::cout << " !!! gridZLc " << gridZLc[0] << std::endl;
    for(int i=0;i<nR_flowV;i++)
    {
        flowVGridr[i] = gridRLc[i];
    }
    for(int i=0;i<nZ_flowV;i++)
    {
        flowVGridz[i] = gridZLc[i];
    }
    std::cout << " !!! flowvgridr0 " << flowVGridr[0] << std::endl;
    int nFlowVs = nR_Lc*nZ_Lc;
    #if LC_INTERP == 3
    int nY_flowV=nY_Lc;
    sim::Array<float> flowVGridy(nY_flowV);//=gridYLc;
    for(int i=0;i<nY_flowV;i++)
        flowVGridy[i] = gridYLc[i];
    nFlowVs = nR_Lc*nY_Lc*nZ_Lc;
    #endif
    sim::Array<float> flowVr(nFlowVs), flowVz(nFlowVs),
                        flowVt(nFlowVs);
    float thisY = 0.0;
    float cs0=0.0;
    float teLocal = 0.0;
    float tiLocal = 0.0;
    float BLocal[3] = {0.0,0.0,0.0};
    float Bnorm[3] = {0.0,0.0,0.0};
    float Bmag = 0.0;
    int index = 0;
    float cs = 0.0;
    float absS = 0.0;
    std::cout << "Beginning analytic flowV calculation "<< std::endl; 
    for(int i=0;i<nR_Lc; i++)
    {
     #if LC_INTERP == 3
     for(int k=0;k < nY_Lc; k++)
     { thisY = flowVGridy[k];
     #endif

         for(int j=0;j<nZ_Lc;j++)
      { 
        //std::cout << "debug here 1 " << i << " " << j << std::endl;
        //std::cout << "debug here 2 " << flowVGridr[i] << " " << thisY << " "  
        //    << flowVGridz[j] << " " << nR_Temp << " "<<nZ_Temp << std::endl;
        teLocal = interp2dCombined(flowVGridr[i],thisY,flowVGridz[j],nR_Temp,nZ_Temp, 
                &TempGridr.front(),&TempGridz.front(),&te.front());
        tiLocal = interp2dCombined(flowVGridr[i],thisY,flowVGridz[j],nR_Temp,nZ_Temp, 
                &TempGridr.front(),&TempGridz.front(),&ti.front());
        cs0 = sqrt((teLocal+tiLocal)*1.602e-19/(background_amu*1.66e-27));
        interp2dVector(&BLocal[0],flowVGridr[i],thisY,flowVGridz[j],nR_Bfield,
                    nZ_Bfield,bfieldGridr.data(),bfieldGridz.data(),br.data(),bz.data(),by.data());
        Bmag = sqrt(BLocal[0]*BLocal[0] + BLocal[1]*BLocal[1] + BLocal[2]*BLocal[2]);
        Bnorm[0] = BLocal[0]/Bmag;
        Bnorm[1] = BLocal[1]/Bmag;
        Bnorm[2] = BLocal[2]/Bmag;

     #if LC_INTERP == 3
        index = i+k*nR_Lc + j*nR_Lc*nY_Lc;
        //std::cout << "flowv calc index " << index << std::endl;
#else
        index = i+j*nR_Lc;

#endif
        absS = abs(s[index]);
        cs = cs0*(0.5*Lc[index]/absS - sqrt(0.25*Lc[index]*Lc[index]/absS/absS - 1.0));
        if(std::isnan(cs)) cs = 0.0;
        flowVr[index] = sgn(s[index])*Bnorm[0]*cs;
        flowVt[index] = sgn(s[index])*Bnorm[1]*cs;
        flowVz[index] = sgn(s[index])*Bnorm[2]*cs;
     #if LC_INTERP == 3
      }
     #endif
      }
    }
    std::cout << "Done with initial calculation, beginning sorting" << std::endl;
    sim::Array<float> flowVrSub(nFlowVs), flowVzSub(nFlowVs),
                        flowVySub(nFlowVs);
    sim::Array<int> noIntersectionNearestMax(nFlowVs);
    float surroundingMinimumR = 0.0;
    float surroundingMinimumY = 0.0;
    float surroundingMinimumZ = 0.0;
    int iterIndex = 0;
    for(int i=0; i<nR_Lc;i++)
    {
        std::cout << "i of " << i << " " << nR_Lc << std::endl;
        for(int j=0;j<nY_Lc;j++)
        {
            for(int k=0;k<nZ_Lc;k++)
            {
               index = i+j*nR_Lc + k*nR_Lc*nY_Lc;
               if(noIntersectionNodes[index] ==1)
               {
                   surroundingMinimumR = 0.0;
                   surroundingMinimumY = 0.0;
                   surroundingMinimumZ = 0.0;
                       for(int ii=i-1; ii<i+2;ii++)
                       {
                         for(int jj=j-1;jj<j+2;jj++)
                         {
                           for(int kk=k-1;kk<k+2;kk++)
                           {
                               iterIndex = ii+jj*nR_Lc + kk*nR_Lc*nY_Lc;
                               if(iterIndex > 0 && iterIndex < nFlowVs)
                               {
                               if(noIntersectionNodes[iterIndex] ==0)
                               {
                                 if(abs(flowVr[iterIndex])>abs(surroundingMinimumR))
                                 {
                                   surroundingMinimumR = flowVr[iterIndex];
                                 }
                                 if(abs(flowVt[iterIndex])>abs(surroundingMinimumY))
                                 {
                                   surroundingMinimumY = flowVt[iterIndex];
                                 }
                                 if(abs(flowVz[iterIndex])>abs(surroundingMinimumZ))
                                 {
                                   surroundingMinimumZ = flowVz[iterIndex];
                                 }
                               }
                               }
                           }
                         }
                       }
                  flowVrSub[index] = surroundingMinimumR; 
                  flowVySub[index] = surroundingMinimumY; 
                  flowVzSub[index] = surroundingMinimumZ; 

               }
            }
        }
    }
    for(int i=0;i<nFlowVs;i++)
    {
            if(i ==282839)
            {
                std::cout<< " noIntersectionNodes " << noIntersectionNodes[i] << std::endl;
            }
               if(noIntersectionNodes[i] ==1)
               {
                  flowVr[i] =  flowVrSub[i];
                  flowVt[i] =  flowVySub[i];
                  flowVz[i] =  flowVzSub[i];
               }

    }

        std::cout << "Finished analytic flowV calculation "<< std::endl; 
  #elif FLOWV_INTERP == 2
    int nR_flowV;
    int nZ_flowV;
    
    const char *flowVFile,*flowVNr,*flowVNz,*flowVGridR,
               *flowVGridZ,*flowVrChar,*flowVzChar,*flowVtChar;
    if(cfg.lookupValue("backgroundPlasmaProfiles.FlowVelocity.fileString", flowVFile) &&
       cfg.lookupValue("backgroundPlasmaProfiles.FlowVelocity.gridNrString",flowVNr) &&
       cfg.lookupValue("backgroundPlasmaProfiles.FlowVelocity.gridNzString",flowVNz) &&
       cfg.lookupValue("backgroundPlasmaProfiles.FlowVelocity.gridRString",flowVGridR) &&
       cfg.lookupValue("backgroundPlasmaProfiles.FlowVelocity.gridZString",flowVGridZ) &&
       cfg.lookupValue("backgroundPlasmaProfiles.FlowVelocity.flowVrString",flowVrChar) &&
       cfg.lookupValue("backgroundPlasmaProfiles.FlowVelocity.flowVzString",flowVzChar) &&
       cfg.lookupValue("backgroundPlasmaProfiles.FlowVelocity.flowVtString",flowVtChar))
    { std::cout << "flowV file: " << flowVFile << std::endl;}
    else
    { std::cout << "ERROR: Could not get flowV string info from input file " << std::endl;}
    int f1 = read_profileNs(flowVFile,flowVNr,flowVNz,nR_flowV,nZ_flowV);
    
    sim::Array<float> flowVGridr(nR_flowV), flowVGridz(nZ_flowV);
    sim::Array<float> flowVr(nR_flowV*nZ_flowV), flowVz(nR_flowV*nZ_flowV),
                        flowVt(nR_flowV*nZ_flowV);
    
    int f2 = read_profile1d(flowVFile,flowVGridR, flowVGridr);
    
    int f3 = read_profile1d(flowVFile,flowVGridZ, flowVGridz);
    
    int f4 = read_profile2d(flowVFile,flowVrChar, flowVr);
    
    int f5 = read_profile2d(flowVFile,flowVzChar, flowVz);
    
    int f6 = read_profile2d(flowVFile,flowVtChar, flowVt);
  #endif

  std::string outnameFlowVr = "flowVr.m";
  std::string outnameFlowVz = "flowVz.m";
  std::string outnameFlowVt = "flowVt.m";
 #if LC_INTERP == 3
  OUTPUT3d(profiles_folder,outnameFlowVr, nR_flowV,nY_flowV, nZ_flowV, &flowVr.front());
  OUTPUT3d(profiles_folder,outnameFlowVz, nR_flowV,nY_flowV, nZ_flowV, &flowVz.front());
  OUTPUT3d(profiles_folder,outnameFlowVt, nR_flowV,nY_flowV, nZ_flowV, &flowVt.front());
#else
  OUTPUT2d(profiles_folder,outnameFlowVr, nR_flowV, nZ_flowV, &flowVr.front());
  OUTPUT2d(profiles_folder,outnameFlowVz, nR_flowV, nZ_flowV, &flowVz.front());
  OUTPUT2d(profiles_folder,outnameFlowVt, nR_flowV, nZ_flowV, &flowVt.front());
#endif
  //Background plasma temperature gradient field intitialization    
  #if GRADT_INTERP == 0
    int nR_gradT = 1;
    int nZ_gradT = 1;
    sim::Array<float> gradTGridr(nR_gradT), gradTGridz(nZ_gradT);
    sim::Array<float> gradTeR(nR_gradT*nZ_gradT), gradTeZ(nR_gradT*nZ_gradT),
        gradTeT(nR_gradT*nZ_gradT,0.0),gradTiR(nR_gradT*nZ_gradT), 
        gradTiZ(nR_gradT*nZ_gradT),gradTiT(nR_gradT*nZ_gradT,0.0);    
    if(cfg.lookupValue("backgroundPlasmaProfiles.gradT.gradTeR", gradTeR[0]) && 
       cfg.lookupValue("backgroundPlasmaProfiles.gradT.gradTeZ", gradTeZ[0]) &&
       cfg.lookupValue("backgroundPlasmaProfiles.gradT.gradTiR", gradTiR[0]) &&
       cfg.lookupValue("backgroundPlasmaProfiles.gradT.gradTiZ", gradTiZ[0]))
    {std::cout << "Temperature gradients TeR and TeZ = " << gradTeR[0] << " " << 
                  gradTeZ[0] << "\nTemperature gradients TiR and TiZ = " << 
                  gradTiR[0] << " " << gradTiZ[0] << std::endl;}
    else
    {std::cout << "ERROR: Failed importing constant temperature gradients" << std:: endl;}
  #elif GRADT_INTERP == 2
    int nR_gradT;
    int nZ_gradT;
    
    const char *gradTFile,*gradTNr,*gradTNz,*gradTGridR,
               *gradTGridZ,*gradTiRChar,*gradTiZChar,*gradTeRChar,*gradTeZChar;
    if(cfg.lookupValue("backgroundPlasmaProfiles.gradT.fileString", gradTFile) &&
       cfg.lookupValue("backgroundPlasmaProfiles.gradT.gridNrString",gradTNr) &&
       cfg.lookupValue("backgroundPlasmaProfiles.gradT.gridNzString",gradTNz) &&
       cfg.lookupValue("backgroundPlasmaProfiles.gradT.gridRString",gradTGridR) &&
       cfg.lookupValue("backgroundPlasmaProfiles.gradT.gridZString",gradTGridZ) &&
       cfg.lookupValue("backgroundPlasmaProfiles.gradT.gradTiRString",gradTiRChar) &&
       cfg.lookupValue("backgroundPlasmaProfiles.gradT.gradTiZString",gradTiZChar) &&
       cfg.lookupValue("backgroundPlasmaProfiles.gradT.gradTeRString",gradTeRChar) &&
       cfg.lookupValue("backgroundPlasmaProfiles.gradT.gradTeZString",gradTeZChar))
    { std::cout << "gradT file: " << gradTFile << std::endl;}
    else
    { std::cout << "ERROR: Could not get gradT string info from input file " << std::endl;}
    int g1 = read_profileNs(gradTFile,gradTNr,gradTNz,nR_gradT,nZ_gradT);
    
    sim::Array<float> gradTGridr(nR_gradT), gradTGridz(nZ_gradT);
    sim::Array<float> gradTeR(nR_gradT*nZ_gradT), gradTeZ(nR_gradT*nZ_gradT),
        gradTeT(nR_gradT*nZ_gradT,0.0),gradTiR(nR_gradT*nZ_gradT), 
        gradTiZ(nR_gradT*nZ_gradT),gradTiT(nR_gradT*nZ_gradT,0.0);
    
    int g2 = read_profile1d(gradTFile,gradTGridR, gradTGridr);
    
    int g3 = read_profile1d(gradTFile,gradTGridZ, gradTGridz);
    
    int g4 = read_profile2d(gradTFile,gradTiRChar, gradTiR);
    
    int g5 = read_profile2d(gradTFile,gradTiZChar, gradTiZ);
    
    int g6 = read_profile2d(gradTFile,gradTeRChar, gradTeR);
    
    int g7 = read_profile2d(gradTFile,gradTeZChar, gradTeZ);
  #endif

  std::string outnameGradTiR = "gradTiR.m";
  std::string outnameGradTiZ = "gradTiZ.m";
  std::string outnameGradTeR = "gradTeR.m";
  std::string outnameGradTeZ = "gradTeZ.m";
  OUTPUT2d(profiles_folder,outnameGradTiR, nR_gradT, nZ_gradT, &gradTiR.front());
  OUTPUT2d(profiles_folder,outnameGradTiZ, nR_gradT, nZ_gradT, &gradTiZ.front());
  OUTPUT2d(profiles_folder,outnameGradTeR, nR_gradT, nZ_gradT, &gradTeR.front());
  OUTPUT2d(profiles_folder,outnameGradTeZ, nR_gradT, nZ_gradT, &gradTeZ.front());


  //Initialization of ionization and recombination coefficients    
  int nCS_Ionize, nCS_Recombine;
  const char *ionizeFile,*ionizeNcs,*ionizeNDens,*ionizeNTemp,
             *ionizeDensGrid,*ionizeTempGrid,*ionizeRCvarChar,
             *recombFile,*recombNcs,*recombNDens,*recombNTemp,
             *recombDensGrid,*recombTempGrid,*recombRCvarChar;
  if(cfg.lookupValue("impurityParticleSource.ionization.fileString", ionizeFile) &&
     cfg.lookupValue("impurityParticleSource.ionization.nChargeStateString",ionizeNcs) &&
     cfg.lookupValue("impurityParticleSource.ionization.DensGridString",ionizeNDens) &&
     cfg.lookupValue("impurityParticleSource.ionization.TempGridString",ionizeNTemp) &&
     cfg.lookupValue("impurityParticleSource.ionization.TempGridVarName",ionizeTempGrid) &&
     cfg.lookupValue("impurityParticleSource.ionization.DensGridVarName",ionizeDensGrid) &&
     cfg.lookupValue("impurityParticleSource.ionization.CoeffVarName",ionizeRCvarChar))
  { std::cout << "Ionization rate coefficient file: " << ionizeFile << std::endl;}
  else
  { std::cout << "ERROR: Could not get ionization string info from input file " << std::endl;}
  if(cfg.lookupValue("impurityParticleSource.recombination.fileString", recombFile) &&
     cfg.lookupValue("impurityParticleSource.recombination.nChargeStateString",recombNcs) &&
     cfg.lookupValue("impurityParticleSource.recombination.DensGridString",recombNDens) &&
     cfg.lookupValue("impurityParticleSource.recombination.TempGridString",recombNTemp) &&
     cfg.lookupValue("impurityParticleSource.recombination.TempGridVarName",recombTempGrid) &&
     cfg.lookupValue("impurityParticleSource.recombination.DensGridVarName",recombDensGrid) &&
     cfg.lookupValue("impurityParticleSource.recombination.CoeffVarName",recombRCvarChar))
  { std::cout << "Recombination rate coefficient file: " << recombFile << std::endl;}
  else
  { std::cout << "ERROR: Could not get ionization string info from input file " << std::endl;}
  int i0 = read_profileNs(ionizeFile,ionizeNcs,recombNcs,nCS_Ionize, nCS_Recombine);

  int nTemperaturesIonize, nDensitiesIonize;
  int i1 = read_profileNs(ionizeFile,ionizeNDens,ionizeNTemp,nDensitiesIonize,nTemperaturesIonize);

  sim::Array<float> rateCoeff_Ionization(nCS_Ionize*nTemperaturesIonize*nDensitiesIonize);
  sim::Array<float> gridTemperature_Ionization(nTemperaturesIonize),
                        gridDensity_Ionization(nDensitiesIonize);

  int i2 = read_profiles(ionizeFile,nTemperaturesIonize,nDensitiesIonize,ionizeTempGrid, 
                         gridTemperature_Ionization,ionizeDensGrid,gridDensity_Ionization,
                         ionizeRCvarChar,rateCoeff_Ionization);
   
  int nTemperaturesRecombine, nDensitiesRecombine;
  int i3 = read_profileNs(recombFile,recombNDens,recombNTemp,
                          nDensitiesRecombine,nTemperaturesRecombine);

  sim::Array<float> rateCoeff_Recombination(nCS_Recombine*nTemperaturesRecombine*nDensitiesRecombine);
  sim::Array<float> gridTemperature_Recombination(nTemperaturesRecombine),
                    gridDensity_Recombination(nDensitiesRecombine);

  int i4 = read_profiles(recombFile,nTemperaturesRecombine,nDensitiesRecombine,
             recombTempGrid,gridTemperature_Recombination,recombDensGrid,
             gridDensity_Recombination,
             recombRCvarChar,rateCoeff_Recombination);


  //Applying background values at material boundaries
  std::for_each(boundaries.begin(), boundaries.end()-1,
            boundary_init(background_Z,background_amu,
            nR_Dens,nZ_Dens,DensGridr.data(),DensGridz.data(),ni.data(),
            nR_Bfield,nZ_Bfield,bfieldGridr.data(),
            bfieldGridz.data(),br.data(),bz.data(), by.data(),
            nR_Temp,nZ_Temp,TempGridr.data(),
            TempGridz.data(),ti.data(), biasPotential ));

   std::cout << "Completed Boundary Init " << std::endl;
  
  //Efield
  #if USEPRESHEATHEFIELD > 0    

   std::cout << "Using presheath Efield " << std::endl;
    #if PRESHEATH_INTERP == 0
      int nR_PreSheathEfield = 1;
      int nZ_PreSheathEfield = 1;
      sim::Array<float> preSheathEGridr(nR_PreSheathEfield), preSheathEGridz(nZ_PreSheathEfield);
      sim::Array<float> PSEr(nR_PreSheathEfield*nZ_PreSheathEfield), 
          PSEz(nR_PreSheathEfield*nZ_PreSheathEfield),
          PSEt(nR_PreSheathEfield*nZ_PreSheathEfield);
      PSEr[0] = cfg.lookup("backgroundPlasmaProfiles.Efield.Er");
      PSEz[0] = cfg.lookup("backgroundPlasmaProfiles.Efield.Ez");
      PSEt[0] = cfg.lookup("backgroundPlasmaProfiles.Efield.Et");
      std::cout << "PSEz " << PSEz[0] << std::endl;
    #elif PRESHEATH_INTERP == 1
    
    int nR_PreSheathEfield=nR_Lc;
    int nZ_PreSheathEfield=nZ_Lc;
    sim::Array<float> preSheathEGridr=gridRLc;
    sim::Array<float> preSheathEGridz=gridZLc;
    int nPSEs = nR_Lc*nZ_Lc;
    #if LC_INTERP == 3
    int nY_PreSheathEfield=nY_Lc;
    sim::Array<float> preSheathEGridy=gridYLc;
    nPSEs = nR_Lc*nY_Lc*nZ_Lc;
    #endif
    std::cout << "length of PSE vec " << nPSEs << std::endl;
    sim::Array<float> PSEr(nPSEs), PSEz(nPSEs),PSEt(nPSEs);
    float teLocal1 = 0.0;
    float BLocal1[3] = {0.0,0.0,0.0};
    float Bnorm1[3] = {0.0,0.0,0.0};
    float Bmag1 = 0.0;
    int index1 = 0;
    float absS1 = 0.0;
    float Epar = 0.0;
    for(int i=0;i<nR_Lc; i++)
    {
     #if LC_INTERP == 3
     for(int k=0;k < nY_Lc; k++)
     { thisY = flowVGridy[k];
     #endif
      for(int j=0;j<nZ_Lc;j++)
      { 
        teLocal1 = interp2dCombined(gridRLc[i],0.0,gridZLc[j],nR_Temp,nZ_Temp, 
                &TempGridr.front(),&TempGridz.front(),&te.front());
        interp2dVector(&BLocal1[0],gridRLc[i],0.0,gridZLc[j],nR_Bfield,
                    nZ_Bfield,bfieldGridr.data(),bfieldGridz.data(),br.data(),bz.data(),by.data());
        Bmag1 = sqrt(BLocal1[0]*BLocal1[0] + BLocal1[1]*BLocal1[1] + BLocal1[2]*BLocal1[2]);
        Bnorm1[0] = BLocal1[0]/Bmag1;
        Bnorm1[1] = BLocal1[1]/Bmag1;
        Bnorm1[2] = BLocal1[2]/Bmag1;

     #if LC_INTERP == 3
        index1 = i+k*nR_Lc + j*nR_Lc*nY_Lc;
        //std::cout << "flowv calc index " << index << std::endl;
#else
        index1 = i+j*nR_Lc;
#endif
        absS1 = abs(s[index1]);
        Epar = teLocal1*(0.5*Lc[index1]/absS1/sqrt(0.25*Lc[index1]*Lc[index1]-absS1*absS1)-1.0/absS1);
        if(std::isnan(Epar)) Epar = 0.0;
        PSEr[index1] = sgn(s[index1])*Bnorm1[0]*Epar;
        PSEt[index1] = sgn(s[index1])*Bnorm1[1]*Epar;
        PSEz[index1] = sgn(s[index1])*Bnorm1[2]*Epar;
      }
     #if LC_INTERP == 3
     }     
#endif
    }
    sim::Array<float> PSErSub(nFlowVs), PSEzSub(nFlowVs),
                        PSEySub(nFlowVs);

    for(int i=0; i<nR_Lc;i++)
    {
        for(int j=0;j<nY_Lc;j++)
        {
            for(int k=0;k<nZ_Lc;k++)
            {
               index = i+j*nR_Lc + k*nR_Lc*nY_Lc;
               if(noIntersectionNodes[index] ==1)
               {
                   surroundingMinimumR = 0.0;
                   surroundingMinimumY = 0.0;
                   surroundingMinimumZ = 0.0;
                       for(int ii=i-1; ii<i+2;ii++)
                       {
                         for(int jj=j-1;jj<j+2;jj++)
                         {
                           for(int kk=k-1;kk<k+2;kk++)
                           {
                               iterIndex = ii+jj*nR_Lc + kk*nR_Lc*nY_Lc;
                               if(iterIndex > 0 && iterIndex < nFlowVs)
                               {
                               if(noIntersectionNodes[iterIndex] ==0)
                               {
                                 if(abs(PSEr[iterIndex])>abs(surroundingMinimumR))
                                 {
                                   surroundingMinimumR = PSEr[iterIndex];
                                 }
                                 if(abs(PSEt[iterIndex])>abs(surroundingMinimumY))
                                 {
                                   surroundingMinimumY = PSEt[iterIndex];
                                 }
                                 if(abs(PSEz[iterIndex])>abs(surroundingMinimumZ))
                                 {
                                   surroundingMinimumZ = PSEz[iterIndex];
                                 }
                               }
                               }
                           }
                         }
                       }
                  PSErSub[index] = surroundingMinimumR; 
                  PSEySub[index] = surroundingMinimumY; 
                  PSEzSub[index] = surroundingMinimumZ; 

               }
            }
        }
    }
    for(int i=0;i<nFlowVs;i++)
    {
            if(i ==282839)
            {
                std::cout<< " noIntersectionNodes " << noIntersectionNodes[i] << std::endl;
            }
               if(noIntersectionNodes[i] ==1)
               {
                  PSEr[i] =  PSErSub[i];
                  PSEt[i] =  PSEySub[i];
                  PSEz[i] =  PSEzSub[i];
               }

    }
    #elif PRESHEATH_INTERP == 2
      int nR_PreSheathEfield;
      int nZ_PreSheathEfield;
      
      const char *PSEFile,*PSENr,*PSENz,*PSEGridR,
                 *PSEGridZ,*PSErChar,*PSEzChar,*PSEtChar;
      if(cfg.lookupValue("backgroundPlasmaProfiles.Efield.fileString", PSEFile) &&
         cfg.lookupValue("backgroundPlasmaProfiles.Efield.gridNrString",PSENr) &&
         cfg.lookupValue("backgroundPlasmaProfiles.Efield.gridNzString",PSENz) &&
         cfg.lookupValue("backgroundPlasmaProfiles.Efield.gridRString",PSEGridR) &&
         cfg.lookupValue("backgroundPlasmaProfiles.Efield.gridZString",PSEGridZ) &&
         cfg.lookupValue("backgroundPlasmaProfiles.Efield.radialComponentString",PSErChar) &&
         cfg.lookupValue("backgroundPlasmaProfiles.Efield.axialComponentString",PSEzChar) &&
         cfg.lookupValue("backgroundPlasmaProfiles.Efield.toroidalComponentString",PSEtChar))
      { std::cout << "PS Electric field file: " << PSEFile << std::endl;}
      else
      { std::cout << "ERROR: Could not get PSE string info from input file " << std::endl;}
      int e1 = read_profileNs(PSEfile,PSENr,PSENz,nR_PreSheathEfield,nZ_PreSheathEfield);
      
      sim::Array<float> preSheathEGridr(nR_PreSheathEfield), preSheathEGridz(nZ_PreSheathEfield);
      sim::Array<float> PSEr(nR_PreSheathEfield*nZ_PreSheathEfield), 
          PSEz(nR_PreSheathEfield*nZ_PreSheathEfield),
          PSEt(nR_PreSheathEfield*nZ_PreSheathEfield,0.0);
      
      int e2 = read_profile1d(PSEFile,PSEGridR, preSheathEGridr);
      
      int e3 = read_profile1d(PSEFile,PSEGridZ, preSheathEGridz);
      
      int e4 = read_profile2d(PSEFile,PSErChar, PSEr);
      
      int e5 = read_profile2d(PSEFile,PSEzChar, PSEz);
      
      //int e6 = read_profile2d(PSEFile,PSEtChar, PSEt);
    #endif
    
    std::string outnamePSEfieldR = "PSEfieldR.m";
    std::string outnamePSEfieldZ = "PSEfieldZ.m";
    std::string outnamePSEGridR = "PSEgridR.m";
    std::string outnamePSEGridZ = "PSEgridZ.m";
    OUTPUT1d(profiles_folder,outnamePSEGridR, nR_PreSheathEfield, &preSheathEGridr.front());
    OUTPUT1d(profiles_folder,outnamePSEGridZ, nZ_PreSheathEfield, &preSheathEGridz.front());
     #if LC_INTERP == 3
    OUTPUT3d(profiles_folder,outnamePSEfieldR, nR_PreSheathEfield,nY_PreSheathEfield, nZ_PreSheathEfield, &PSEr.front());
    OUTPUT3d(profiles_folder,outnamePSEfieldZ, nR_PreSheathEfield,nY_PreSheathEfield, nZ_PreSheathEfield, &PSEz.front());
     #else
    OUTPUT2d(profiles_folder,outnamePSEfieldR, nR_PreSheathEfield, nZ_PreSheathEfield, &PSEr.front());
    OUTPUT2d(profiles_folder,outnamePSEfieldZ, nR_PreSheathEfield, nZ_PreSheathEfield, &PSEz.front());
     #endif  
#else
    
      int nR_PreSheathEfield = 1;
      int nY_PreSheathEfield = 1;
      int nZ_PreSheathEfield = 1;
      int closestBoundaryIndex;
      sim::Array<float> preSheathEGridr(nR_PreSheathEfield),preSheathEGridy(nR_PreSheathEfield), preSheathEGridz(nZ_PreSheathEfield);
      sim::Array<float> PSEr(nR_PreSheathEfield*nZ_PreSheathEfield), 
          PSEz(nR_PreSheathEfield*nZ_PreSheathEfield),
          PSEt(nR_PreSheathEfield*nZ_PreSheathEfield);
  #endif
    
  std::cout << "Completed presheath Efield Init " << std::endl;
  sim::Array<float> Efieldr(nR_Bfield*nZ_Bfield), Efieldz(nR_Bfield*nZ_Bfield),
                    Efieldt(nR_Bfield*nZ_Bfield),minDist(nR_Bfield*nZ_Bfield);

  #if USESHEATHEFIELD > 0
    #if EFIELD_INTERP == 1
      float thisE[3] = {0.0,0.0,0.0};
    
      for(int i=0;i<nR_Bfield;i++)
      {
         for(int j=0;j<nZ_Bfield;j++)
         {
             minDist[(nR_Bfield - 1 -i)*nZ_Bfield+(nZ_Bfield -1-j)] = 
                  getE ( bfieldGridr[i], 0.0, bfieldGridz[j],
                  thisE, boundaries.data(),nLines,closestBoundaryIndex );
             Efieldr[i*nZ_Bfield+j] = thisE[0];
             Efieldz[i*nZ_Bfield+j] = thisE[2];
             Efieldt[i*nZ_Bfield+j] = thisE[1];
          }
      }
        
      int nR_closeGeom;
      int nZ_dtsEfield;
      
      int d1 = read_profileNs(cfg.lookup("backgroundPlasmaProfiles.dtsEfield.fileString"),
                  cfg.lookup("backgroundPlasmaProfiles.dtsEfield.gridNrString"),
                  cfg.lookup("backgroundPlasmaProfiles.dtsEfield.gridNzString"),nR_dtsEfield,nZ_dtsEfield);
      
      sim::Array<float> dtsEfieldGridr(nR_dtsEfield), dtsEfieldGridz(nZ_dtsEfield);
      sim::Array<float> dtsE(nR_dtsEfield*nZ_dtsEfield);
      
      int d2 = read_profile1d(cfg.lookup("backgroundPlasmaProfiles.dtsEfield.fileString"),
                  cfg.lookup("backgroundPlasmaProfiles.dtsEfield.gridRString"), dtsEfieldGridr);
      
      std::cout << "got first grid " << dtsEfieldGridr.front() << std::endl;    
      int d3 = read_profile1d(cfg.lookup("backgroundPlasmaProfiles.dtsEfield.fileString"),
                  cfg.lookup("backgroundPlasmaProfiles.dtsEfield.gridZString"), dtsEfieldGridz);
      
      std::cout << "got second grid" << dtsEfieldGridz.front() << std::endl;    
      
      int d4 = read_profile2d(cfg.lookup("backgroundPlasmaProfiles.dtsEfield.fileString"),
                  cfg.lookup("backgroundPlasmaProfiles.dtsEfield.sheathDTS"), dtsE);
    #elif EFIELD_INTERP ==2
        int nR_dtsEfield, nZ_dtsEfield;
        
        int d1 = read_profileNs(cfg.lookup("backgroundPlasmaProfiles.dtsEfield.fileString"),
                    cfg.lookup("backgroundPlasmaProfiles.dtsEfield.gridNrString"),
                    cfg.lookup("backgroundPlasmaProfiles.dtsEfield.gridNzString"),
                    nR_dtsEfield,nZ_dtsEfield);
        
        sim::Array<float> dtsEfieldGridr(nR_dtsEfield), dtsEfieldGridz(nZ_dtsEfield);
        sim::Array<float> dtsE(nR_dtsEfield*nZ_dtsEfield);
        
        int d2 = read_profile1d(cfg.lookup("backgroundPlasmaProfiles.dtsEfield.fileString"),
                    cfg.lookup("backgroundPlasmaProfiles.dtsEfield.gridRString"), dtsEfieldGridr);
        
        int d3 = read_profile1d(cfg.lookup("backgroundPlasmaProfiles.dtsEfield.fileString"),
                    cfg.lookup("backgroundPlasmaProfiles.dtsEfield.gridZString"), dtsEfieldGridz);
        
        int d4 = read_profile2d(cfg.lookup("backgroundPlasmaProfiles.dtsEfield.fileString"),
                    cfg.lookup("backgroundPlasmaProfiles.dtsEfield.sheathDTS"), dtsE);
    #endif
  #else
    int nR_dtsEfield=1;
    int nZ_dtsEfield=1;
    sim::Array<float> dtsEfieldGridr(nR_dtsEfield), dtsEfieldGridz(nZ_dtsEfield);
    sim::Array<float> dtsE(nR_dtsEfield*nZ_dtsEfield);
  #endif

  std::string outnameEfieldR = "EfieldR.m";
  std::string outnameEfieldZ = "EfieldZ.m";
  std::string outnameEfieldT = "EfieldT.m";
  std::string outnameMinDist = "DistToSurface.m";
  OUTPUT2d(profiles_folder,outnameEfieldR, nR_Bfield, nZ_Bfield, &Efieldr.front());
  OUTPUT2d(profiles_folder,outnameEfieldZ, nR_Bfield, nZ_Bfield, &Efieldz.front());
  OUTPUT2d(profiles_folder,outnameEfieldT, nR_Bfield, nZ_Bfield, &Efieldt.front());
  OUTPUT2d(profiles_folder,outnameMinDist, nR_Bfield, nZ_Bfield, &minDist.front());

#if SPECTROSCOPY > 0
    float netX0=0.0,netX1=0.0,netY0=0.0,netY1=0.0,netZ0=0.0,netZ1=0.0;
    int net_nX=0,net_nY=0,net_nZ=0;
    int nBins=0;

    if(cfg.lookupValue("diagnostics.netx0", netX0) && 
       cfg.lookupValue("diagnostics.netx1", netX1) && 
       cfg.lookupValue("diagnostics.nety0", netY0) && 
       cfg.lookupValue("diagnostics.nety1", netY1) && 
       cfg.lookupValue("diagnostics.netz0", netZ0) && 
       cfg.lookupValue("diagnostics.netz1", netZ1) && 
       cfg.lookupValue("diagnostics.nX", net_nX) && 
       cfg.lookupValue("diagnostics.nY", net_nY) && 
       cfg.lookupValue("diagnostics.nZ", net_nZ) && 
       cfg.lookupValue("diagnostics.densityChargeBins", nBins))
       {std::cout << "Spectroscopy net imported" << std::endl;}
    else
    { std::cout << "ERROR: Could not get spectroscopy net string info from input file " << std::endl;}
    std::cout << "spec bin Ns " << net_nX << " " << net_nY << " " << net_nZ << std::endl; 
    #if SPECTROSCOPY < 3

      sim::Array<float> net_Bins((nBins+1)*net_nX*net_nZ);
    #else
      sim::Array<float> net_Bins((nBins+1)*net_nX*net_nY*net_nZ);
    #endif

      /*
      for (int i=0; i<nBins*net_nX*net_nZ; i++)
          {
              std::cout << "i " << i << std::endl;
            net_Bins[i] = 0;
              std::cout << "net bins " << net_Bins[i] << std::endl;
            
          }
      */
      sim::Array<float> gridX_bins(net_nX),gridY_bins(net_nY),gridZ_bins(net_nZ);

      for (int i=0; i< net_nX ; i++)
      {
         gridX_bins[i] = netX0 + 1.0/(net_nX-1)*i*(netX1-netX0);
      }
      for (int i=0; i< net_nY ; i++)
      {
         gridY_bins[i] = netY0 + 1.0/(net_nY-1)*i*(netY1-netY0);
      }

      for (int i=0; i< net_nZ ; i++)
      {
         gridZ_bins[i] = netZ0 + i*1.0/(net_nZ-1)*(netZ1-netZ0);
      }
  #endif    

  // Perp DiffusionCoeff initialization - only used when Diffusion interpolator is = 0
  float perpDiffusionCoeff;
  if (cfg.lookupValue("backgroundPlasmaProfiles.Diffusion.Dperp",perpDiffusionCoeff))
  {}
  else
  {std::cout << "ERROR: could not get perpendicular diffusion coefficient from input file" << std::endl;}

  // Particle time stepping control
  int ionization_nDtPerApply  = cfg.lookup("timeStep.ionization_nDtPerApply");
  int collision_nDtPerApply  = cfg.lookup("timeStep.collision_nDtPerApply");

  #ifdef __CUDACC__
    cout<<"Using THRUST"<<endl;
  #else
    cout<<"Not using THRUST"<<endl;
    int nthreads, tid;
    #pragma omp parallel private(nthreads, tid)
    {
        nthreads = omp_get_num_threads();
          tid = omp_get_thread_num();
          if(tid == 0)
          {
              std::cout << "N Threads " << nthreads << std::endl;
          }
          std::cout << "Hello world" << tid << std::endl;
    }
        //nthreads = omp_get_num_threads();
        nthreads = 24;
        std::cout << "N threads " << nthreads << std::endl;
    thrust::counting_iterator<std::size_t> ex0(0);  
    thrust::counting_iterator<std::size_t> ex1(nthreads-1);
                  thrust::for_each(thrust::device, ex0,ex1,
                                   ompPrint());
  #endif

  float dt;
  const int nP = cfg.lookup("impurityParticleSource.nP");
  long nParticles = nP;
  int nT;

  if (cfg.lookupValue("timeStep.dt",dt) &&
      cfg.lookupValue("timeStep.nT",nT))    
  {
  cout << "Number of time steps: " << nT << " With dt = " << dt << endl; 
  cout << "Number of particles: " << nP << endl;              
  }
  else
  {std::cout << "ERROR: could not get nT, dt, or nP from input file" << std::endl;}

  auto particleArray = new Particles(nParticles);
  
  #if PARTICLE_SOURCE == 0
    float x,y,z,Ex,Ey,Ez,amu,Z,charge;
    if (cfg.lookupValue("impurityParticleSource.initialConditions.x_start",x) &&
        cfg.lookupValue("impurityParticleSource.initialConditions.y_start",y) &&
        cfg.lookupValue("impurityParticleSource.initialConditions.z_start",z) &&
        cfg.lookupValue("impurityParticleSource.initialConditions.energy_eV_x_start",Ex) &&
        cfg.lookupValue("impurityParticleSource.initialConditions.energy_eV_y_start",Ey) &&
        cfg.lookupValue("impurityParticleSource.initialConditions.energy_eV_z_start",Ez) &&  
        cfg.lookupValue("impurityParticleSource.initialConditions.impurity_amu",amu) && 
        cfg.lookupValue("impurityParticleSource.initialConditions.impurity_Z",Z) &&
        cfg.lookupValue("impurityParticleSource.initialConditions.charge",charge))
    { std::cout << "Impurity point source: " << x << " " << y << " " << z << std::endl;
    }
    else
    { std::cout << "ERROR: Could not get point source impurity initial conditions" << std::endl;}
    
    for (int i=0; i< nP ; i++)
    {
      particleArray->setParticle(i,x, y, z, Ex, Ey, Ez, Z, amu, charge);
    }
  #elif PARTICLE_SOURCE == 1
    float x;
    float y;
    float z;
    
    float Ex;
    float Ey;
    float Ez;
    
    float amu;
    float Z;
    float charge;
    float impurity_Z = cfg.lookup("impurityParticleSource.Z");
    int nImpurityBoundaries = 0;
    for (int i=0; i<nLines;i++)
    {
        if(boundaries[i].Z == impurity_Z)
        {
            nImpurityBoundaries++;
        }
    }
    std::cout << "n Impurity Boundaries to launch from " << nImpurityBoundaries << std::endl;
    sim::Array<int> boundaryIndex_ImpurityLaunch(nImpurityBoundaries);
    
    int count = 0;
    for (int i=0; i<nLines;i++)
    {
        if(boundaries[i].Z == impurity_Z)
        {
            boundaryIndex_ImpurityLaunch[count] = i;
            count++;
            std::cout << "Boundary indices " << i << std::endl;
        }
    }
    
    int impuritiesPerBoundary = nP/nImpurityBoundaries;
      
    std::uniform_real_distribution<float> distributionForSeeds(0,1e6);
    #if FIXEDSEEDS ==0
        std::random_device randDevice;
        std::default_random_engine generator0(randDevice());
    #else
        float randDevice = 6.5298E+5;
        std::default_random_engine generator0(randDevice);
    #endif
    
    sim::Array<float> boundarySeeds0(4*nImpurityBoundaries);
    std::generate( boundarySeeds0.begin(), boundarySeeds0.end(), [&]() { return distributionForSeeds(generator0); } );
    std::uniform_real_distribution<float> dist01(0.0, 1.0);
    float rand0 = 0.0;
    float rand1 = 0.0;
    float rand2 = 0.0;
    float rand3 = 0.0;

    sim::Array<std::mt19937> s0(4*nImpurityBoundaries);
    
    float E0 = 0.0;
//Create Thompson Distribution
    float surfaceBindingEnergy = cfg.lookup("impurityParticleSource.source_material_SurfaceBindingEnergy");
    std::cout << "surface binding energy " << surfaceBindingEnergy << std::endl;
    int nThompDistPoints = 200;
    float max_Energy = 100.0;
    sim::Array<float> ThompsonDist(nThompDistPoints),CumulativeDFThompson(nThompDistPoints);
    for(int i=0;i<nThompDistPoints;i++)
        {
            ThompsonDist[i] = (i*max_Energy/nThompDistPoints)/pow((i*max_Energy/nThompDistPoints) + surfaceBindingEnergy,3);
            if(i==0)
            {
                CumulativeDFThompson[i] = ThompsonDist[i]; 
            }
            else
            {
                CumulativeDFThompson[i] = CumulativeDFThompson[i-1]+ThompsonDist[i];
            }
        }
    for(int i=0;i<nThompDistPoints;i++)
        {
            CumulativeDFThompson[i] = CumulativeDFThompson[i]/CumulativeDFThompson[nThompDistPoints-1];
            //std::cout << "energy and CDF" << i*max_Energy/nThompDistPoints << " " << CumulativeDFThompson[i] << std::endl;
        }

    for(int j=0; j<4*nImpurityBoundaries;j++)
        {
            std::mt19937  s(boundarySeeds0[j]);
            s0[j] = s;
        }
    // Particle p1(0.0,0.0,0.0,0.0,0.0,0.0,0,0.0);
    for (int i=0; i< nImpurityBoundaries;i++)
    {
        for(int j=0; j<impuritiesPerBoundary; j++)
        {
            //Set boundary interval, properties, and random number gen
        if (i==0)
        {
            rand0 = dist01(s0[0]);
            x = boundaries[boundaryIndex_ImpurityLaunch[i]].x1 + 
                boundaries[boundaryIndex_ImpurityLaunch[i]].length*rand0;//1.4290;
            //std::cout << "start pos 1 " << x << std::endl;
            z = -1.2540+0.00001;
            rand1 = dist01(s0[1]);
            rand2 = dist01(s0[2]);
            rand3 = dist01(s0[3]);
            E0 = interp1dUnstructured(rand2,nThompDistPoints, max_Energy, &CumulativeDFThompson.front());
            Ex = E0*cos(3.1415*rand1)*sin(3.1415*rand3);
            Ey = E0*cos(3.1415*rand3);
            Ez = E0*sin(3.1415*rand1)*sin(3.1415*rand3);
        }
        else
        {
            rand0 = dist01(s0[4]);
            x = boundaries[boundaryIndex_ImpurityLaunch[i]].x1 + boundaries[boundaryIndex_ImpurityLaunch[i]].length*rand0;
            //x = 1.3450;
            //std::cout << "start pos 2 " << x << std::endl;
            z = -1.3660+0.00001;
            rand1 = dist01(s0[5]);
            rand2 = dist01(s0[6]);
            rand3 = dist01(s0[7]);
            E0 = interp1dUnstructured(rand2,nThompDistPoints, max_Energy, &CumulativeDFThompson.front());
            Ex = E0*cos(3.1415*rand1)*sin(3.1415*rand3);
            Ey = E0*cos(3.1415*rand3);
            Ez = E0*sin(3.1415*rand1)*sin(3.1415*rand3);
        }
        particleArray->setParticle((i * impuritiesPerBoundary + j),x, 0.0, z, Ex, Ey, Ez, 74, 18400.0, charge);            
        }
    }
  #elif PARTICLE_SOURCE == 2
    std::cout << "Read particle source " << std::endl;
    Config cfg_particles;
    cfg_particles.readFile("particleSource.cfg");
    Setting& particleSource = cfg_particles.lookup("particleSource");
    std::cout << "found setting particleSource " << std::endl;
    float rSample;
    float angleSample;
    float x;
    float y;
    float z;
    
    float Vr;
    float Vx;
    float Vy;
    float Vz;
    float V0;
    float E0;
    float amu;
    float Z;
    float charge= cfg.lookup("impurityParticleSource.initialConditions.charge");
    float impurity_Z = cfg.lookup("impurityParticleSource.Z");
    int nSources = particleSource["nSources"];
    int nSegments = particleSource["nSegments"];
    int nSegmentsAngle = particleSource["nSegmentsAngle"];
    int cylSymm = particleSource["cylSymm"];
    
    sim::Array<float> sourceR(2*nSources);
    sim::Array<float> sourceZ(2*nSources);
    sim::Array<float> sourceRsegments(nSegments);
    sim::Array<float> sourceZsegments(nSegments);
    sim::Array<float> spaceCDF(nSegments);
    sim::Array<float> sourceAngleSegments(nSegmentsAngle);
    sim::Array<float> angleCDF(nSegmentsAngle);
    
    for (int i=0; i<(nSources*2); i++)
    {
        sourceR[i] = particleSource["r0"][i];
        sourceZ[i] = particleSource["z0"][i];
    }
    
    for (int i=0; i<(nSegments); i++)
    {
        sourceRsegments[i] = particleSource["r"][i];
        sourceZsegments[i] = particleSource["z"][i];
        spaceCDF[i] = particleSource["spaceCDF"][i];
    }
    
    for (int i=0; i<(nSegmentsAngle); i++)
    {
        sourceAngleSegments[i] = particleSource["angles"][i];
        angleCDF[i] = particleSource["angleCDF"][i];
    }
    std::uniform_real_distribution<float> dist01(0.0, 1.0);
    float rand0 = 0.0;
    float rand1 = 0.0;
    float rand2 = 0.0;
    float rand3 = 0.0;
    float rand4 = 0.0;

    sim::Array<std::mt19937> s0(5);
    std::mt19937 ss0(123314.234);
     s0[0] =  ss0;
    std::mt19937 ss1(389362.735);
     s0[1] =  ss1;
    std::mt19937 ss2(523563.108);
     s0[2] =  ss2;
    std::mt19937 ss3(752081.751);
     s0[3] =  ss3;
    std::mt19937 ss4(952381.034);
     s0[4] =  ss4;

    float surfaceBindingEnergy = cfg.lookup("impurityParticleSource.source_material_SurfaceBindingEnergy");
    std::cout << "surface binding energy " << surfaceBindingEnergy << std::endl;
    int nThompDistPoints = 200;
    float max_Energy = 100.0;
    sim::Array<float> ThompsonDist(nThompDistPoints),CumulativeDFThompson(nThompDistPoints);
    for(int i=0;i<nThompDistPoints;i++)
    {
       ThompsonDist[i] = (i*max_Energy/nThompDistPoints)/pow((i*max_Energy/nThompDistPoints) + surfaceBindingEnergy,3);
       if(i==0)
       {
           CumulativeDFThompson[i] = ThompsonDist[i]; 
       }
       else
       {
           CumulativeDFThompson[i] = CumulativeDFThompson[i-1]+ThompsonDist[i];
       }
    }
    for(int i=0;i<nThompDistPoints;i++)
    {
       CumulativeDFThompson[i] = CumulativeDFThompson[i]/CumulativeDFThompson[nThompDistPoints-1];
       //std::cout << "energy and CDF" << i*max_Energy/nThompDistPoints << " " << CumulativeDFThompson[i] << std::endl;
    }

    //rand0 = dist01(s0[0]);
    //rand1 = dist01(s0[1]);
    //rand2 = dist01(s0[2]);
    //rand3 = dist01(s0[3]);
    sim::Array<float> angleBins(180);
    int angleBinNum = 0;
    sim::Array<float> angleBins2(180);
    int angleBinNum2 = 0;
    int closestBoundaryIndex0;
    float minDist0;
    float thisE0[3] = {0.0,0.0,0.0};
    for(int j=0; j<nParticles ; j++)
    {
      rand0 = dist01(s0[0]);
      rSample = interp1dUnstructured2(rand0,nSegments,&sourceRsegments.front() , &spaceCDF.front());
      rand4 = dist01(s0[4]);
      x = rSample*cos(rand4*2.0*3.1415);
      y = rSample*sin(rand4*2.0*3.1415);
      z = sourceZ[0];
      rand1 = dist01(s0[1]);
      E0 = interp1dUnstructured(rand1,nThompDistPoints, max_Energy, &CumulativeDFThompson.front());
      V0 = sqrt(2*E0*1.602e-19/(184.0*1.66e-27));
      rand2 = dist01(s0[2]);
      angleSample = interp1dUnstructured2(rand2,nSegmentsAngle,&sourceAngleSegments.front() , &angleCDF.front());
      angleBinNum=floor(angleSample*180/3.1415);
      angleBins[angleBinNum] = angleBins[angleBinNum]+1;
      //std::cout << angleSample << std::endl;
      //Ey = //E0*cos(angleSample)*sin(2.0*3.1415*rand3);
      Vz = V0*cos(angleSample);
      Vr = V0*sin(angleSample);//cos(2.0*3.1415*rand3)
      //std::cout << "Ez " << Ez << " Er " << Er << std::endl; 
      rand3 = dist01(s0[3]);
      //rand3 = j*1.0/nParticles;
      Vx = Vr*cos(2.0*3.1415*rand3);
      Vy = Vr*sin(2.0*3.1415*rand3);//E0*cos(angleSample)*sin(2.0*3.1415*rand3);
      angleBinNum2=floor(rand3*180.0);
      angleBins2[angleBinNum2] = angleBins2[angleBinNum2]+1;
      //std::cout << "rsample " << rSample << " E0 " << E0 << " angleSample " << angleSample << std::endl;
    particleArray->setParticleV(j,x,y,z,Vx,Vy,Vz,74, 184.0, charge);
       minDist0 = getE ( x,y,z,thisE0, boundaries.data(),nLines,

                        nR_closeGeom_sheath,nY_closeGeom_sheath,nZ_closeGeom_sheath,n_closeGeomElements_sheath,
                        &closeGeomGridr_sheath.front(),&closeGeomGridy_sheath.front(),&closeGeomGridz_sheath.front(),
                        &closeGeom_sheath.front(),
                  closestBoundaryIndex0 );
       //std::cout << "closest Boundary " << x << " " << y << " " << z <<" " 
       //          << closestBoundaryIndex0 << std::endl;
       boundaries[closestBoundaryIndex0].startingParticles = boundaries[closestBoundaryIndex0].startingParticles + 1.0;
    }
  #endif

  std::cout << "finished loading particle source" << std::endl;
  #if USESURFACEMODEL > 0
    //import text file
      std::vector<float> spylGridE,spylGridAngle,spylGridRoughness,spyl;
      std::string line;
      std::string delimiter = " ";
      size_t pos = 0;
      std::string token;
      ifstream myfile ("cumulativeEA.txt");
      int counter = 0;
      int header = 0;
      if (myfile.is_open())
      {
        while ( getline (myfile,line) )
        {
          cout << line << '\n';
          counter = 0;
          if(header == 0)
          {
              header++;
          }
          else
          {
            while ((pos = line.find(delimiter)) != std::string::npos) 
            {
                  token = line.substr(0, pos);
                  if(token.empty()){}
                  else
                  {    
                      counter++;
                      std::cout <<counter << " " << token << std::endl;
                      if(header == 1)
                      {
                        spylGridE.push_back(atof(token.c_str()));
                      }
                      else if(header == 2)
                      {
                        spylGridAngle.push_back(atof(token.c_str()));
                      }
                      else if (header == 3)
                      {
                        spylGridRoughness.push_back(atof(token.c_str()));
                      }
                      else{
                      if(counter == 1)
                      {  
                        //spylGridE.push_back(atof(token.c_str()));
                      }
                      else if(counter==2)
                      {
                        //spylGridAngle.push_back(atof(token.c_str()));
                      }
                      else if(counter==3)
                      {
                        //spylGridRoughness.push_back(atof(token.c_str()));
                      }
                      else if(counter==4)
                      {
                        spyl.push_back(atof(token.c_str()));
                      }
                      }
                  }
                          line.erase(0, pos + delimiter.length());
            }
            header++;
          }
          std::cout << line << std::endl;
        }
        myfile.close();
      }

      else cout << "Unable to open file";

      for(int i=0;i<spylGridE.size();i++)
      {
          std::cout << "spylGridE " << spylGridE[i] << std::endl;
          //std::cout << "spylGrida " << spylGridAngle[i] << std::endl;
          //std::cout << "spylGridr " << spylGridRoughness[i] << std::endl;
          //std::cout << "spyl " << spyl[i] << std::endl;
      }
      for(int i=0;i<spylGridAngle.size();i++)
      {
          //std::cout << "spylGridE " << spylGridE[i] << std::endl;
          std::cout << "spylGrida " << spylGridAngle[i] << std::endl;
          //std::cout << "spylGridr " << spylGridRoughness[i] << std::endl;
          //std::cout << "spyl " << spyl[i] << std::endl;
      }
      for(int i=0;i<spylGridRoughness.size();i++)
      {
          //std::cout << "spylGridE " << spylGridE[i] << std::endl;
          //std::cout << "spylGrida " << spylGridAngle[i] << std::endl;
          std::cout << "spylGridr " << spylGridRoughness[i] << std::endl;
          //std::cout << "spyl " << spyl[i] << std::endl;
      }
      int nAngle = spylGridAngle.size();
      int nEnergy = spylGridE.size();
      //std::cout << "interp spyl " << spyl[2+1*nAngle] << " " << spyl[1] << std::endl;
      //float temp21 = 0.0;
      //temp21 = interp2dUnstructured(15.0,120.0,nAngle,nEnergy, 
      //        &spylGridAngle.front(),&spylGridE.front(), &spyl.front());
      //std::cout << "interp spyl 15, 120 " << temp21 << std::endl;
      sim::Array<float> spYlGridAngle(nAngle);
      sim::Array<float> spYlGridE(nEnergy);
      sim::Array<float> spYl(nAngle*nEnergy);
      for(int i=0;i<nAngle;i++)
      {
        spYlGridAngle[i] = spylGridAngle[i];
      }
      for(int i=0;i<nEnergy;i++)
      {
        spYlGridE[i] = spylGridE[i];
      }
      for(int i=0;i<nAngle*nEnergy;i++)
      {
        spYl[i] = spyl[i];
      }
      //spYlGridAngle = spylGridAngle;
      //spYlGridE = spYlGridE;
      //spYl = spyl;
  #endif

  #if GEOM_TRACE > 0       
    std::uniform_real_distribution<float> dist2(0,1);
    //std::random_device rd2;
    //std::default_random_engine generator2(rd2());
    float randDevice02 = 6.52E+5;
    std::default_random_engine generator2(randDevice02);
    std::cout << "Randomizing velocities to trace geometry. " << std::endl;

    for (int i=0 ; i<nParticles ; i++)
    {   float theta = dist2(generator2)*2*3.1415;
        float phi = dist2(generator2)*3.1415;
        float mag = 2e3;
        particleArray->vx[i] = mag*cos(theta)*sin(phi);
        particleArray->vy[i] = mag*sin(theta)*sin(phi);
        particleArray->vz[i] = mag*cos(phi);
    }
  #endif

  #if PARTICLE_TRACKS > 0
    int subSampleFac = 1;
    #if USE_CUDA > 0
      sim::Array<float> positionHistoryX(nP*nT/subSampleFac);
      sim::Array<float> positionHistoryY(nP*nT/subSampleFac);
      sim::Array<float> positionHistoryZ(nP*nT/subSampleFac);
      sim::Array<float> velocityHistoryX(nP*nT/subSampleFac);
      sim::Array<float> velocityHistoryY(nP*nT/subSampleFac);
      sim::Array<float> velocityHistoryZ(nP*nT/subSampleFac);
      sim::Array<float> chargeHistory(nP*nT/subSampleFac);
    #else
      float **positionHistoryX;
      float **positionHistoryY;
      float **positionHistoryZ;
      float **velocityHistoryX;
      float **velocityHistoryY;
      float **velocityHistoryZ;
      float **chargeHistory;
      positionHistoryX = new float* [nP];
      positionHistoryY = new float* [nP];
      positionHistoryZ = new float* [nP];
      velocityHistoryX = new float* [nP];
      velocityHistoryY = new float* [nP];
      velocityHistoryZ = new float* [nP];
      chargeHistory = new float* [nP];
      positionHistoryX[0] = new float [nT*nP/subSampleFac];
      positionHistoryY[0] = new float [nT*nP/subSampleFac];
      positionHistoryZ[0] = new float [nT*nP/subSampleFac];
      velocityHistoryX[0] = new float [nT*nP/subSampleFac];
      velocityHistoryY[0] = new float [nT*nP/subSampleFac];
      velocityHistoryZ[0] = new float [nT*nP/subSampleFac];
      chargeHistory[0] = new float [nT*nP/subSampleFac];
      for(int i=0 ; i<nP ; i++)
      {
          positionHistoryX[i] = &positionHistoryX[0][i*nT/subSampleFac];
          positionHistoryY[i] = &positionHistoryY[0][i*nT/subSampleFac];
          positionHistoryZ[i] = &positionHistoryZ[0][i*nT/subSampleFac];
          velocityHistoryX[i] = &velocityHistoryX[0][i*nT/subSampleFac];
          velocityHistoryY[i] = &velocityHistoryY[0][i*nT/subSampleFac];
          velocityHistoryZ[i] = &velocityHistoryZ[0][i*nT/subSampleFac];
          chargeHistory[i] = &chargeHistory[0][i*nT/subSampleFac];
          for(int j=0 ; j<nT/subSampleFac ; j++)
          {
              positionHistoryX[i][j] = 0.0;
              positionHistoryY[i][j] = 0.0;
              positionHistoryZ[i][j] = 0.0;
              velocityHistoryX[i][j] = 0.0;
              velocityHistoryY[i][j] = 0.0;
              velocityHistoryZ[i][j] = 0.0;
              chargeHistory[i][j] = 0.0;
          }
      }
    #endif
  #endif 
  float* finalPosX = new float[nP];
  float* finalPosY = new float[nP];
  float* finalPosZ = new float[nP];
  float* finalVx = new float[nP];
  float* finalVy = new float[nP];
  float* finalVz = new float[nP];
  float* transitTime = new float[nP];
  float* hitWall = new float[nP];
  #if USE_BOOST
    //cpu_timer timer;
  #endif

  std::cout << "beginning seeds" << std::endl;
  std::uniform_real_distribution<float> dist(0,1e6);

  #if FIXEDSEEDS == 0
    std::random_device rd;
    std::default_random_engine generator(rd());
    std::default_random_engine generator1(rd());
    std::default_random_engine generator2(rd());
    std::default_random_engine generator3(rd());
    std::default_random_engine generator4(rd());
    std::default_random_engine generator5(rd());
    std::default_random_engine generator6(rd());
  #endif

  thrust::counting_iterator<std::size_t> particleBegin(0);  
  thrust::counting_iterator<std::size_t> particleEnd(nParticles);

  #if PARTICLESEEDS > 0
    #if USEIONIZATION > 0
      #if FIXEDSEEDS ==1
        std::cout << "ionization fixed seeds" << std::endl;
        float ionization_seeds = cfg.lookup("operators.ionization.seed");
        std::default_random_engine generator(ionization_seeds);
      #endif
      sim::Array<float> seeds0(nP);
      std::generate( seeds0.begin(), seeds0.end(), [&]() { return dist(generator); } );
      thrust::transform(thrust::device, particleArray->streams.begin(),  
                        particleArray->streams.end(),seeds0.begin(), 
                        particleArray->streams.begin(), randInit(0) );
    #endif

    #if USERECOMBINATION > 0
      #if FIXEDSEEDS ==1
        std::cout << "recombination fixed seeds" << std::endl;
        float recombination_seeds = cfg.lookup("operators.recombination.seed");
        std::cout << "recombination fixed seeds middle" << std::endl;
        std::default_random_engine generator1(recombination_seeds);
        std::cout << "recombination fixed seeds end" << std::endl;
      #endif
      sim::Array<float> seeds1(nP);
      std::cout << "generate" << std::endl;
      #if __CUDACC__
        cudaDeviceSynchronize();
      #endif
      std::generate( seeds1.begin(), seeds1.end(), [&]() { return dist(generator1); } );
      std::cout << "transform" << std::endl;
      #if __CUDACC__
        cudaDeviceSynchronize();
      #endif
      thrust::transform(thrust::device,particleArray->streams_rec.begin(), particleArray->streams_rec.end(),
                    seeds1.begin(), particleArray->streams_rec.begin(), randInit(1) );
      std::cout << "finished transform" << std::endl;
    #endif

    #if USEPERPDIFFUSION > 0
      #if FIXEDSEEDS ==1
        std::cout << "diffusion fixed seeds" << std::endl;
        float diffusion_seeds = cfg.lookup("operators.perpDiffusion.seed");
        std::default_random_engine generator2(diffusion_seeds);
      #endif
      sim::Array<float> seeds2(nP);
#if USE_CUDA  
      cudaDeviceSynchronize();
#endif
      std::generate( seeds2.begin(), seeds2.end(), [&]() { return dist(generator2); } );
      #if __CUDACC__
        cudaDeviceSynchronize();
      #endif
      thrust::transform(thrust::device,particleArray->streams_diff.begin(), particleArray->streams_diff.end(),
                    seeds2.begin(), particleArray->streams_diff.begin(), randInit(2) );
#if USE_CUDA  
        cudaDeviceSynchronize();
#endif
#endif

    #if USECOULOMBCOLLISIONS > 0
      #if FIXEDSEEDS ==1
        std::cout << "collision fixed seeds" << std::endl;
        float collision_seeds1 = cfg.lookup("operators.coulombCollisions.seed1");
        float collision_seeds2 = cfg.lookup("operators.coulombCollisions.seed2");
        float collision_seeds3 = cfg.lookup("operators.coulombCollisions.seed3");
        std::cout << "debug 1" << std::endl;
#if USE_CUDA  
        cudaDeviceSynchronize();
#endif
        std::default_random_engine generator3(collision_seeds1);
        std::cout << "debug 2" << std::endl;
#if USE_CUDA  
        cudaDeviceSynchronize();
#endif
        std::default_random_engine generator4(collision_seeds2);
#if USE_CUDA  
        cudaDeviceSynchronize();
#endif
        std::cout << "debug 3" << std::endl;
        std::default_random_engine generator5(collision_seeds3);
      #endif
#if USE_CUDA  
        cudaDeviceSynchronize();
#endif
        std::cout << "debug 4" << std::endl;
      sim::Array<float> seeds3(nP),seeds4(nP),seeds5(nP);
#if USE_CUDA  
        cudaDeviceSynchronize();
#endif
        std::generate( seeds3.begin(), seeds3.end(), [&]() { return dist(generator3); } );
#if USE_CUDA  
        cudaDeviceSynchronize();
#endif
        std::generate( seeds4.begin(), seeds4.end(), [&]() { return dist(generator4); } );
#if USE_CUDA  
        cudaDeviceSynchronize();
#endif
        std::generate( seeds5.begin(), seeds5.end(), [&]() { return dist(generator5); } );
#if USE_CUDA  
      cudaDeviceSynchronize();
#endif
      std::cout << "debug 5" << std::endl;
      thrust::transform(thrust::device,particleArray->streams_collision1.begin(), 
                                       particleArray->streams_collision1.end(),
                                       seeds3.begin(), 
                                       particleArray->streams_collision1.begin(), randInit(3) );
        std::cout << "debug 6" << std::endl;
#if USE_CUDA  
        cudaDeviceSynchronize();
#endif
        thrust::transform(thrust::device,particleArray->streams_collision2.begin(), 
                                       particleArray->streams_collision2.end(),
                                       seeds4.begin(),
                                       particleArray->streams_collision2.begin(), randInit(4) );
#if USE_CUDA  
        cudaDeviceSynchronize();
#endif
        std::cout << "debug 7" << std::endl;
#if USE_CUDA  
      cudaDeviceSynchronize();
#endif
      thrust::transform(thrust::device,particleArray->streams_collision3.begin(),
                                       particleArray->streams_collision3.end(),
                                       seeds5.begin(), 
                                       particleArray->streams_collision3.begin(), randInit(5) );
#if USE_CUDA  
        cudaDeviceSynchronize();
#endif
        std::cout << "debug 8" << std::endl;
    #endif
   std::cout<< "finished collision seeds" << std::endl;
    #if USESURFACEMODEL > 0
      #if FIXEDSEEDS ==1
        std::cout << "surface model fixed seeds" << std::endl;
        float surface_seeds = cfg.lookup("operators.surfaceModel.seed");
        std::default_random_engine generator6(surface_seeds);
      #endif
      sim::Array<float> seeds6(nP);
#if USE_CUDA  
      cudaDeviceSynchronize();
#endif
      std::generate( seeds6.begin(), seeds6.end(), [&]() { return dist(generator6); } );
#if USE_CUDA  
      cudaDeviceSynchronize();
#endif
      thrust::transform(thrust::device,particleArray->streams_surf.begin(), particleArray->streams_surf.end(),
                    seeds6.begin(), particleArray->streams_surf.begin(), randInit(6) );
#if USE_CUDA  
      cudaDeviceSynchronize();
#endif
#endif

    std::cout << "at empty defns" << std::endl;
    #if __CUDACC__
      sim::Array<curandState> state1(7);//Definition empty for passing to module
    #else
      sim::Array<std::mt19937> state1(7);//Empty definition
    #endif

    std::cout << "finished empty defns" << std::endl;
  #else //ParticleSeeds == 0

    #if __CUDACC__
      sim::Array<curandState> state1(9);
      curandInitialize<<<1,1>>>(&state1[0],19);
      curandInitialize<<<1,1>>>(&state1[1],25);
      curandInitialize<<<1,1>>>(&state1[2],32);
      curandInitialize<<<1,1>>>(&state1[3],39);
      curandInitialize<<<1,1>>>(&state1[4],43);
      curandInitialize<<<1,1>>>(&state1[5],48);
      curandInitialize<<<1,1>>>(&state1[6],51);
      curandInitialize<<<1,1>>>(&state1[7],56);
      curandInitialize<<<1,1>>>(&state1[8],60);
    #else
      sim::Array<std::mt19937> state1(9);
      std::mt19937 s0(348763);
      std::mt19937 s1(358763);
      std::mt19937 s2(346763);
      std::mt19937 s3(348263);
      std::mt19937 s4(349763);
      std::mt19937 s5(318763);
      std::mt19937 s6(448763);
      std::mt19937 s7(448963);
      std::mt19937 s8(463763);
      state1[0] = s0;
      state1[1] = s1;
      state1[2] = s2;
      state1[3] = s3;
      state1[4] = s4;
      state1[5] = s5;
      state1[6] = s6;
      state1[7] = s7;
      state1[8] = s8;
    #endif
  #endif

    std::cout << "at movetime" << std::endl;
    float moveTime = 0.0;
    float geomCheckTime = 0.0;
    float ionizTime = 0.0;

#if USE_BOOST
    //cpu_times copyToDeviceTime = timer.elapsed();
    //std::cout << "Initialize rand state and copyToDeviceTime: " << copyToDeviceTime.wall*1e-9 << '\n';
#endif
    typedef std::chrono::high_resolution_clock Time;
    typedef std::chrono::duration<float> fsec;
    auto start_clock = Time::now();
    std::cout << "Starting main loop" << std::endl;
    //std:: cout << "particle value " << particleArray->hitWall[0] << std::endl;
//Main time loop
    #if __CUDACC__
      cudaDeviceSynchronize();
    #endif
    for(int tt=0; tt< nT; tt++)
    {
#ifdef __CUDACC__
    cudaThreadSynchronize();
#endif
        thrust::for_each(thrust::device, particleBegin,particleEnd, 
                move_boris(particleArray,dt,boundaries.data(), nLines,
                    nR_Bfield,nZ_Bfield, bfieldGridr.data(),&bfieldGridz.front(),
                    &br.front(),&bz.front(),&by.front(),
                    nR_PreSheathEfield,nY_PreSheathEfield,nZ_PreSheathEfield,
                    &preSheathEGridr.front(),&preSheathEGridy.front(),&preSheathEGridz.front(),
                    &PSEr.front(),&PSEz.front(),&PSEt.front(),
                        nR_closeGeom_sheath,nY_closeGeom_sheath,nZ_closeGeom_sheath,n_closeGeomElements_sheath,
                        &closeGeomGridr_sheath.front(),&closeGeomGridy_sheath.front(),&closeGeomGridz_sheath.front(),
                        &closeGeom_sheath.front()) );
        
        
        //try {
            thrust::for_each(thrust::device, particleBegin,particleEnd,
                    geometry_check(particleArray,nLines,&boundaries[0],dt,tt,
                        nR_closeGeom,nY_closeGeom,nZ_closeGeom,n_closeGeomElements,
                        &closeGeomGridr.front(),&closeGeomGridy.front(),&closeGeomGridz.front(),
                        &closeGeom.front()) );
       // }
       /*
            catch (thrust::system_error &e) {
            std::cerr << "Thrust system error: " << e.what() << std::endl;
            exit(-1);
        }
        */
#if SPECTROSCOPY > 0
            thrust::for_each(thrust::device, particleBegin,particleEnd,
                    spec_bin(particleArray,nBins,net_nX,net_nY, net_nZ, &gridX_bins.front(),&gridY_bins.front(),
                        &gridZ_bins.front(), &net_Bins.front(),dt) );
#endif            
#if USEIONIZATION > 0
        thrust::for_each(thrust::device, particleBegin,particleEnd,
                ionize(particleArray, dt,&state1.front(),
                    nR_Dens,nZ_Dens,&DensGridr.front(),&DensGridz.front(),&ne.front(),  
                    nR_Temp,nZ_Temp,&TempGridr.front(),&TempGridz.front(),&te.front(),
                    nTemperaturesIonize, nDensitiesIonize,&gridTemperature_Ionization.front(),
                    &gridDensity_Ionization.front(), &rateCoeff_Ionization.front(),tt));
#endif
#if USERECOMBINATION > 0
        thrust::for_each(thrust::device, particleBegin,particleEnd,
                recombine(particleArray, dt,&state1.front(),
                    nR_Dens,nZ_Dens,&DensGridr.front(),&DensGridz.front(),&ne.front(),  
                    nR_Temp,nZ_Temp,&TempGridr.front(),&TempGridz.front(),&te.front(),
                    nTemperaturesRecombine,nDensitiesRecombine,
                    gridTemperature_Recombination.data(),gridDensity_Recombination.data(),
                    rateCoeff_Recombination.data(),tt));
#endif
#if USEPERPDIFFUSION > 0
        thrust::for_each(thrust::device,particleBegin, particleEnd,
                crossFieldDiffusion(particleArray,dt,&state1.front(),perpDiffusionCoeff,
                    nR_Bfield,nZ_Bfield,bfieldGridr.data(),&bfieldGridz.front(),
                                        &br.front(),&bz.front(),&by.front()));
            
            thrust::for_each(thrust::device, particleBegin,particleEnd,
                    geometry_check(particleArray,nLines,&boundaries[0],dt,tt,
                        nR_closeGeom,nY_closeGeom,nZ_closeGeom,n_closeGeomElements,
                        &closeGeomGridr.front(),&closeGeomGridy.front(),&closeGeomGridz.front(),
                        &closeGeom.front()) );
#endif
#if USECOULOMBCOLLISIONS > 0
        thrust::for_each(thrust::device, particleBegin, particleEnd, 
                coulombCollisions(particleArray,dt,&state1.front(),
                    nR_flowV,nZ_flowV,&flowVGridr.front(),&flowVGridz.front(),
                    &flowVr.front(),&flowVz.front(),&flowVt.front(),
                    nR_Dens,nZ_Dens,&DensGridr.front(),&DensGridz.front(),&ne.front(),    
                    nR_Temp,nZ_Temp,&TempGridr.front(),&TempGridz.front(),&te.front(),
                    background_Z,background_amu, 
                    nR_Bfield,nZ_Bfield,bfieldGridr.data(),&bfieldGridz.front(),
                                        &br.front(),&bz.front(),&by.front()));

#endif
#if USETHERMALFORCE > 0
        thrust::for_each(thrust::device,particleBegin, particleEnd,
                thermalForce(particleArray,dt,background_amu,
                    nR_gradT,nZ_gradT,gradTGridr.data(),gradTGridz.data(),
                    gradTiR.data(),gradTiZ.data(), gradTiT.data(), 
                    gradTeR.data(), gradTeZ.data(), gradTeT.data(), 
                    nR_Bfield,nZ_Bfield, bfieldGridr.data(),&bfieldGridz.front(),
                    &br.front(),&bz.front(),&by.front()));
#endif

#if USESURFACEMODEL > 0
        thrust::for_each(thrust::device,particleBegin, particleEnd, 
                reflection(particleArray,dt,&state1.front(),nLines,&boundaries[0],nAngle,nEnergy,
                      spYlGridAngle.data(),
                            spYlGridE.data(), 
                                  spYl.data(),nSegmentsAngle,&sourceAngleSegments.front() , &angleCDF.front(),
                   nThompDistPoints, max_Energy, &CumulativeDFThompson.front() ) );
#endif        

#if PARTICLE_TRACKS >0
#if USE_CUDA > 0
   thrust::for_each(thrust::device, particleBegin,particleEnd,
      history(particleArray,tt,subSampleFac,nP,&positionHistoryX.front(),
      &positionHistoryY.front(),&positionHistoryZ.front(),
      &velocityHistoryX.front(),&velocityHistoryY.front(),
      &velocityHistoryZ.front(),&chargeHistory.front()) );
#else
if (tt % subSampleFac == 0)  
{    
        for(int i=0;i<nP;i++)
        {
            positionHistoryX[i][tt/subSampleFac] = particleArray->xprevious[i];
            positionHistoryY[i][tt/subSampleFac] = particleArray->yprevious[i];
            positionHistoryZ[i][tt/subSampleFac] = particleArray->zprevious[i];
            velocityHistoryX[i][tt/subSampleFac] = particleArray->vx[i];
            velocityHistoryY[i][tt/subSampleFac] = particleArray->vy[i];
            velocityHistoryZ[i][tt/subSampleFac] = particleArray->vz[i];
            chargeHistory[i][tt/subSampleFac] = particleArray->charge[i];
        }
}
#endif
#endif
    }
// Ensure that all time step loop GPU kernels are complete before proceeding
    #ifdef __CUDACC__
        cudaDeviceSynchronize();
    #endif

    auto finish_clock = Time::now();
    fsec fs = finish_clock - start_clock;
    printf("Time taken          is %6.3f (secs) \n", fs.count());
    printf("Time taken per step is %6.3f (secs) \n", fs.count() / (float) nT);
#if USE_BOOST
    //cpu_times ionizeTimeGPU = timer.elapsed();
    //std::cout << "Particle Moving Time: " << ionizeTimeGPU.wall*1e-9 << '\n';
#endif
    /*
for(int i=0; i<nP ; i++)
{
    std::cout << "particle " << i << " first rnd# " << 
        particleArray->test[i] << " and x " << particleArray->xprevious[i] << 
         " hitwall " << particleArray->hitWall[i] << 
         " trans " << particleArray->transitTime[i] << std::endl;
}
*/
    std::cout << "transit time counting "<< nP << std::endl;
    //float tmp202 =0.0;
#if USE_CUDA
    cudaDeviceSynchronize();
#endif
//    tmp202 =  particleArray->vx[0];
    std::cout << "memory access hitwall " 
    << particleArray->xprevious[0] << std::endl;
    std::cout << "transit time counting " << std::endl;
#if USE3DTETGEOM > 0
    float meanTransitTime0 = 0.0;
    /*
    for (int i=0; i<nP; i++)
    {
        std::cout << "loop " << i << std::endl;
        if(particleArray->hitWall[i] == 1.0)
        {
            meanTransitTime0 = meanTransitTime0 + particleArray->transitTime[i];
        }
    }
    */
meanTransitTime0 = meanTransitTime0/nP;
std::cout << " mean transit time " << meanTransitTime0 << std::endl;
    int max_boundary = 0;
    float max_impacts = 0.0;
    int max_boundary1 = 0;
    float max_impacts1 = 0.0;
    float* impacts = new float[nLines];
    float* startingParticles = new float[nLines];
    for (int i=0; i<nLines; i++)
    {
        impacts[i] = boundaries[i].impacts;
        startingParticles[i] = boundaries[i].startingParticles;
        if (boundaries[i].impacts > max_impacts)
        {
            max_impacts = boundaries[i].impacts;
            max_boundary = i;
        }
    }


std::cout << "maximum boundary " << max_boundary << std::endl;
std::cout << "number of counts " << max_impacts << std::endl;
/*
sim::Array<float> tally00(nLines,0);
for (int j=0; j<nP; j++)
{
    tally00[particleArray->wallHit[j]] = tally00[particleArray->wallHit[j]] + 1;
}

std::cout << "bound 164p " << tally00[164] << std::endl;
std::cout << "bound 255p " << tally00[255] << std::endl;

std::cout << "bound 164 " << boundaries[164].impacts << std::endl;
std::cout << "bound 255 " << boundaries[255].impacts << std::endl;
*/
#else
    float* impacts = new float[nLines];
    float* startingParticles = new float[nLines];
    for (int i=0; i<nLines; i++)
    {
        impacts[i] = boundaries[i].impacts;
        startingParticles[i] = boundaries[i].startingParticles;
    }
#endif
#if PARTICLE_SOURCE == 1
int ring1 = 0;
int ring2 = 0;
int noWall = 0;
float meanTransitTime = 0.0;

for(int i=0; i<nP ; i++)
{
	if(particleArray->wallIndex[i] == boundaryIndex_ImpurityLaunch[0])
	{
		ring1++;
	}
	else if(particleArray->wallIndex[i] == boundaryIndex_ImpurityLaunch[1])
	{
		ring2++;
	}
	
	if(particleArray->wallIndex[i] == 0)
	{
		noWall++;
	}
	
	meanTransitTime = meanTransitTime + particleArray->transitTime[i];
	
} 
meanTransitTime = meanTransitTime/(nP-noWall);
std::cout << "Number of impurity particles deposited on ring 1 " << ring1 << std::endl;
std::cout << "Number of impurity particles deposited on ring 2 " << ring2 << std::endl;
std::cout << "Number of impurity particles not deposited " << noWall << std::endl;
std::cout << "Mean transit time of deposited particles " << meanTransitTime << std::endl;
#endif
    std::cout << "positions.m writing " << std::endl;
    ofstream outfile2;
    outfile2.open ("positions.m");
    for(int i=1 ; i<=nP ; i++)
      {
        outfile2 << "Pos( " << i<< ",:) = [ " ;
        outfile2 << particleArray->x[i-1] << " " << particleArray->y[i-1] 
            << " " << particleArray->z[i-1] << " ];" << std::endl;
      }
       outfile2.close();
// Write netCDF output for positions
for (int i=0; i<nP; i++)
{
    finalPosX[i] = particleArray->xprevious[i];
    finalPosY[i] = particleArray->yprevious[i];
    finalPosZ[i] = particleArray->zprevious[i];
    finalVx[i] =   particleArray->vx[i];
    finalVy[i] =   particleArray->vy[i];
    finalVz[i] =   particleArray->vz[i];
    transitTime[i] = particleArray->transitTime[i];
    hitWall[i] = particleArray->hitWall[i];
}
NcFile ncFile0("positions.nc", NcFile::replace);
NcDim nc_nP0 = ncFile0.addDim("nP",nP);
vector<NcDim> dims0;
dims0.push_back(nc_nP0);

NcVar nc_x0 = ncFile0.addVar("x",ncDouble,dims0);
NcVar nc_y0 = ncFile0.addVar("y",ncDouble,dims0);
NcVar nc_z0 = ncFile0.addVar("z",ncDouble,dims0);
NcVar nc_vx0 = ncFile0.addVar("vx",ncDouble,dims0);
NcVar nc_vy0 = ncFile0.addVar("vy",ncDouble,dims0);
NcVar nc_vz0 = ncFile0.addVar("vz",ncDouble,dims0);
NcVar nc_trans0 = ncFile0.addVar("transitTime",ncDouble,dims0);
NcVar nc_impact0 = ncFile0.addVar("hitWall",ncDouble,dims0);

nc_x0.putVar(finalPosX);
nc_y0.putVar(finalPosY);
nc_z0.putVar(finalPosZ);
nc_vx0.putVar(finalVx);
nc_vy0.putVar(finalVy);
nc_vz0.putVar(finalVz);
nc_trans0.putVar(transitTime);
nc_impact0.putVar(hitWall);

NcFile ncFile1("surface.nc", NcFile::replace);
NcDim nc_nLines = ncFile1.addDim("nLines",nLines);
vector<NcDim> dims1;
dims1.push_back(nc_nLines);

NcVar nc_surfImpacts = ncFile1.addVar("impacts",ncDouble,dims1);
NcVar nc_surfStartingParticles = ncFile1.addVar("startingParticles",ncDouble,dims1);
nc_surfImpacts.putVar(impacts);
nc_surfStartingParticles.putVar(startingParticles);
#if PARTICLE_TRACKS > 0

// Write netCDF output for histories
NcFile ncFile_hist("history.nc", NcFile::replace);
NcDim nc_nT = ncFile_hist.addDim("nT",nT/subSampleFac);
NcDim nc_nP = ncFile_hist.addDim("nP",nP);
vector<NcDim> dims_hist;

#if USE_CUDA
NcDim nc_nPnT = ncFile_hist.addDim("nPnT",nP*nT/subSampleFac);
dims_hist.push_back(nc_nPnT);
#else
NcVar nc_z = ncFile_hist.addVar("z",ncDouble,dims_hist);

NcVar nc_vx = ncFile_hist.addVar("vx",ncDouble,dims_hist);
NcVar nc_vy = ncFile_hist.addVar("vy",ncDouble,dims_hist);
NcVar nc_vz = ncFile_hist.addVar("vz",ncDouble,dims_hist);

NcVar nc_charge = ncFile_hist.addVar("charge",ncDouble,dims_hist);
#if USE_CUDA > 0
float *xPointer = &positionHistoryX[0];
float *yPointer = &positionHistoryY[0];
float *zPointer = &positionHistoryZ[0];
float *vxPointer = &velocityHistoryX[0];
float *vyPointer = &velocityHistoryY[0];
float *vzPointer = &velocityHistoryZ[0];
float *chargePointer = &chargeHistory[0];
nc_x.putVar(xPointer);
nc_y.putVar(yPointer);
nc_z.putVar(zPointer);

nc_vx.putVar(vxPointer);
nc_vy.putVar(vyPointer);
nc_vz.putVar(vzPointer);

nc_charge.putVar(chargePointer);
#else
nc_x.putVar(positionHistoryX[0]);
nc_y.putVar(positionHistoryY[0]);
nc_z.putVar(positionHistoryZ[0]);

nc_vx.putVar(velocityHistoryX[0]);
nc_vy.putVar(velocityHistoryY[0]);
nc_vz.putVar(velocityHistoryZ[0]);

nc_charge.putVar(chargeHistory[0]);
#endif
#endif
#endif
#if SPECTROSCOPY > 0
// Write netCDF output for density data
NcFile ncFile("spec.nc", NcFile::replace);
NcDim nc_nBins = ncFile.addDim("nBins",nBins+1);
NcDim nc_nR = ncFile.addDim("nR",net_nX);
NcDim nc_nY = ncFile.addDim("nY",net_nY);
NcDim nc_nZ = ncFile.addDim("nZ",net_nZ);

vector<NcDim> dims;
dims.push_back(nc_nBins);
dims.push_back(nc_nZ);
dims.push_back(nc_nY);
dims.push_back(nc_nR);

NcVar nc_n = ncFile.addVar("n",ncDouble,dims);
float *binPointer = &net_Bins[0];
nc_n.putVar(binPointer);
#endif
#ifdef __CUDACC__
    cudaThreadSynchronize();
#endif
#if USE_BOOST
    /*
    cpu_times copyToHostTime = timer.elapsed();

    cpu_times createParticlesTimeCPU = timer.elapsed();
    std::cout << "Copy to host, bin and output time: " << (createParticlesTimeCPU.wall-copyToHostTime.wall)*1e-9 << '\n';
    std::cout << "Total ODE integration time: " << moveTime*1e-9 << '\n';
    std::cout << "Total geometry checking time: " << geomCheckTime*1e-9 << '\n';
    std::cout << "Total ionization time: " << ionizTime*1e-9 << '\n';
    */
#endif

#ifdef __CUDACC__
    cudaError_t err = cudaDeviceReset();
//cudaProfilerStop();
#endif
    return 0;
    }
