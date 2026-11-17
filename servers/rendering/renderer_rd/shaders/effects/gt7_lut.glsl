#[compute]
#version 450

// One-time SDR GT7 bake: linear Rec.709 HDR in, linear Rec.709 display RGB out.
// Owned by GT7LUT; exposure and display encoding belong to the tone-mapping pass.
// GT7 transform: Copyright (c) 2025 Polyphony Digital Inc., MIT (Expat).
// Color transforms: Copyright (c) 2020 The Android Open Source Project, Apache-2.0.
// Adapted from google/filament ToneMapper.cpp and ColorSpaceUtils.h.
// Full notices and license text are recorded in COPYRIGHT.txt.
// A 64-thread group also fits mobile devices with small compute limits.
layout(local_size_x = 4, local_size_y = 4, local_size_z = 4) in;
layout(rgba16f, set = 0, binding = 0) uniform restrict writeonly image3D destination;

const mat3 TM_SRGB_XYZ=mat3(.4124560,.2126730,.0193339,.3575760,.7151520,.1191920,.1804380,.0721750,.9503040);
const mat3 TM_XYZ_2020=mat3(1.7166634,-.6666738,.0176425,-.3556733,1.6164557,-.0427770,-.2533681,.0157683,.9422433);
const mat3 TM_2020_XYZ=mat3(.6369530,.2626983,0.,.1446169,.6780088,.0280731,.1688558,.0592929,1.0608272);
const mat3 TM_XYZ_SRGB=mat3(3.2404542,-.9692660,.0556434,-1.5371385,1.8760108,-.2040259,-.4985314,.0415560,1.0572252);

// Encode absolute Rec.2020 LMS luminance in nits using ST 2084 (PQ).
vec3 tmPQ(vec3 x){
	vec3 g=pow(max(x/10000.,vec3(0)),vec3(2610./16384.));
	return pow((3424./4096.+(2413./128.)*g)/(1.+(2392./128.)*g),vec3(2523./32.));
}

// Decode PQ back to absolute luminance for the chroma-preserving branch.
vec3 tmInversePQ(vec3 x){
	vec3 g=pow(clamp(x,0.,1.),vec3(32./2523.));
	return 10000.*pow(max(g-3424./4096.,0.)/((2413./128.)-(2392./128.)*g),vec3(16384./2610.));
}

// Convert linear Rec.2020 nits into perceptual intensity and chroma.
vec3 tmToICtCp(vec3 x){
	vec3 lms=tmPQ(vec3(dot(x,vec3(1688,2146,262)),dot(x,vec3(683,2951,462)),dot(x,vec3(99,309,3688)))/4096.);
	return vec3((lms.x+lms.y)*.5,dot(lms,vec3(6610,-13613,7003))/4096.,dot(lms,vec3(17933,-17390,-543))/4096.);
}

// Reconstruct nonnegative linear Rec.2020 nits from intensity and chroma.
vec3 tmFromICtCp(vec3 x){
	vec3 l=tmInversePQ(vec3(x.x+.00860904*x.y+.11103*x.z,x.x-.00860904*x.y-.11103*x.z,x.x+.560031*x.y-.320627*x.z));
	return max(vec3(dot(l,vec3(3.43661,-2.50645,.0698454)),dot(l,vec3(-.79133,1.9836,-.192271)),dot(l,vec3(-.0259499,-.0989137,1.12486))),0.);
}

// Apply the official SDR toe and exponential highlight shoulder.
float tmGT7Curve(float x){
	const float peak=2.5, alpha=.25, mid=.538, section=.444, toe=1.280;
	const float k=(section-1.)/(alpha-1.);
	const float ka=peak*section+peak*k;
	const float kb=-peak*k*exp(section/k);
	const float kc=-1./(k*peak);
	if(x<0.)return 0.;
	if(x<section*peak){
		float w=smoothstep(0.,mid,x);
		return mix(mid*pow(x/mid,toe),x,w);
	}
	return ka+kb*exp(x*kc);
}

// Blend per-channel shaping with intensity/chroma shaping, then return SDR Rec.709.
vec3 tmGT7(vec3 inputRGB){
	vec3 color=TM_XYZ_2020*TM_SRGB_XYZ*max(inputRGB,0.);
	vec3 ucs=tmToICtCp(color*100.);
	vec3 skewed=vec3(tmGT7Curve(color.r),tmGT7Curve(color.g),tmGT7Curve(color.b));
	vec3 skewUcs=tmToICtCp(skewed*100.);
	float whiteUcs=tmToICtCp(vec3(250.)).x;
	float chroma=1.-smoothstep(.98,1.16,ucs.x/whiteUcs);
	vec3 scaled=tmFromICtCp(vec3(skewUcs.x,ucs.yz*chroma))*.01;
	vec3 output2020=.4*min(mix(skewed,scaled,.6),vec3(2.5));
	return clamp(TM_XYZ_SRGB*TM_2020_XYZ*output2020,0.,1.);
}

void main() {
	ivec3 index = ivec3(gl_GlobalInvocationID);
	if (any(greaterThanEqual(index, ivec3(65)))) {
		return;
	}
	vec3 coordinate = vec3(index) / 64.0;
	vec3 hdr = 0.01 * (exp2(coordinate * log2(1.0 + 8192.0 / 0.01)) - 1.0);
	imageStore(destination, index, vec4(tmGT7(hdr), 1.0));
}
