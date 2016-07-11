.SUFFIXES:
.SUFFIXES: .c .cpp .cu

USECUDA := 1
USEMPI := 0

USEIONIZATION := 0
USERECOMBINATION := 0
USEPERPDIFFUSION := 0
USECOULOMBCOLLISIONS := 0
USETHERMALFORCE :=0
USESURFACEMODEL :=0

#Interpolators 0=Constant 1=Analytic Equation 2=2dInterpolation 3=3dInterpolation
BFIELD_INTERP :=0
EFIELD_INTERP :=0
DENSITY_INTERP :=0
TEMP_INTERP :=0

#Set all above operators to 0 to trace Geometry
GEOM_TRACE :=1

#Particle Source 0 = puffing
PARTICLE_SOURCE :=0

#Set periodic = 0 in gitrGeometry.cfg to use cylindrical symmetry
USECYLSYMM = 1

NAME := bin/GITR

INCLUDEFLAGS :=  
LIBS :=  

CC := gcc
CPP := g++
NVCC := nvcc

MODULES := src include

# Boost

INCLUDEFLAGS+=
LIBS+= -lboost_system -lboost_timer

# Libconfig

INCLUDEFLAGS+= -I $(LIBCONFIGINCLUDE)
LIBS+= -L $(LIBCONFIGDIR) -lconfig++

INCLUDEFLAGS+= -I $(NETCDFCXX4INCLUDE) -I $(NETCDFINCLUDE)
LIBS+= -L $(NETCDFCXX4DIR) -L $(NETCDFDIR) -lnetcdf_c++4 -lnetcdf

#CFLAGS += $(INCLUDEFLAGS) -g
#CXXFLAGS += $(INCLUDEFLAGS) -g
#NVCCFLAGS += $(INCLUDEFLAGS) -g -G

CPPFLAGS:=
CPPFLAGS+= -DUSEMPI=${USEMPI}
CPPFLAGS+= -DUSEIONIZATION=${USEIONIZATION}
CPPFLAGS+= -DUSERECOMBINATION=${USERECOMBINATION}
CPPFLAGS+= -DUSEPERPDIFFUSION=${USEPERPDIFFUSION}
CPPFLAGS+= -DUSECOULOMBCOLLISIONS=${USECOULOMBCOLLISIONS}
CPPFLAGS+= -DUSETHERMALFORCE=${USETHERMALFORCE}
CPPFLAGS+= -DUSESURFACEMODEL=${USESURFACEMODEL}
CPPFLAGS+= -DUSECYLSYMM=${USECYLSYMM}
CPPFLAGS+= -DGEOM_TRACE=${GEOM_TRACE}
CPPFLAGS+= -DPARTICLE_SOURCE=${PARTICLE_SOURCE}
CPPFLAGS+= -std=c++11
#CPPFLAGS+= -DTHRUST_DEBUG

# You shouldn't have to go below here
#
# DLG: 	Added the -x c to force c file type so that 
# 		the .cu files will work too :)

DIRNAME = `dirname $1`
MAKEDEPS = gcc -MM -MG $2 -x c $3 | sed -e "s@^\(.*\)\.o:@.dep/$1/\1.d obj/$1/\1.o:@"
#MAKEDEPS = ${CC} -MM -MG $2 -x c $3 | sed -e "s@^\(.*\)\.o:@.dep/$1/\1.d obj/$1/\1.o:@"

.PHONY : all

all : $(NAME)

# look for include files in each of the modules
INCLUDEFLAGS += $(patsubst %, -I%, $(MODULES))

CFLAGS += $(INCLUDEFLAGS)
CXXFLAGS += $(INCLUDEFLAGS) 
NVCCFLAGS += $(INCLUDEFLAGS) 

# determine the object files
SRCTYPES := c cpp cu
LINK := $(CPP) ${CXXFLAGS} ${LFLAGS}

ifeq ($(USECUDA),1)
LINK := $(NVCC) ${NVCCFLAGS} ${LFLAGS}
else
NVCCFLAGS+= --x c++
endif

OBJ := $(foreach srctype, $(SRCTYPES), $(patsubst %.$(srctype), obj/%.o, $(wildcard $(patsubst %, %/*.$(srctype), $(MODULES)))))

# link the program
$(NAME) : $(OBJ)
	$(LINK) -o $@ $(OBJ) $(LIBS)

# calculate include dependencies
.dep/%.d : %.cpp
	@mkdir -p `echo '$@' | sed -e 's|/[^/]*.d$$||'`
	$(call MAKEDEPS,$(call DIRNAME, $<), $(INCLUDEFLAGS), $<) > $@

obj/%.o : %.cpp
	@mkdir -p `echo '$@' | sed -e 's|/[^/]*.o$$||'`
	$(CPP) $(CXXFLAGS) ${CPPFLAGS} -c -o $@ $<

.dep/%.d : %.c
	@mkdir -p `echo '$@' | sed -e 's|/[^/]*.d$$||'`
	$(call MAKEDEPS,$(call DIRNAME, $<), $(CFLAGS), $<) > $@

obj/%.o : %.c
	@mkdir -p `echo '$@' | sed -e 's|/[^/]*.o$$||'`
	$(CC) $(CFLAGS) -c -o $@ $<

.dep/%.d : %.cu
	@mkdir -p `echo '$@' | sed -e 's|/[^/]*.d$$||'`
	$(call MAKEDEPS,$(call DIRNAME, $<), $(INCLUDEFLAGS), $<) > $@

obj/%.o : %.cu
	@mkdir -p `echo '$@' | sed -e 's|/[^/]*.o$$||'`
	$(NVCC) $(NVCCFLAGS) ${CPPFLAGS} -dc -o $@ $<


# include the C include dependencies
DEP := $(patsubst obj/%.o, .dep/%.d, $(OBJ))

ifneq ($(MAKECMDGOALS),clean)
include $(DEP)
endif

clean:
	-@rm $(NAME) $(OBJ) $(DEP) .dep/src/*

