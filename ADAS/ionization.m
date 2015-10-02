%function z = ionization(z,dt,n,T,State,RateCoeff)
%Syntax for creating multiple random number streams

[s1,s2,s3] = RandStream.create('mrg32k3a','NumStreams',3); %Include ,'Seed','shuffle' to get different values each time

z=1;
T=750;
dt=1e-9;
n=1e15;

Nelem = size(Te(:,z));
diff = Te(:,z)-T;
%{
for i=1:Nelem
    if (diff(i) >=0)
        i;
        break
    end
end

frac = (T-Te(i-1,z))/(Te(i,z)-Te(i-1,z))

Coeff = RateCoeff(i-1,z) + frac*(RateCoeff(i,z) - RateCoeff(i-1,z))
%}
Coeff = interp1(Te(:,z),RateCoeff(:,z),T)

if ( isnan(Coeff) ) 
    error('Ionization interpolation out of range')
end

tion = 1/(Coeff*n)
P1 = 1-exp(-dt/tion)

r1=rand(s1)
 if r1 <= P1
    z = z+1
 end   

r1 = rand(s1)
r1 = rand(s1)

