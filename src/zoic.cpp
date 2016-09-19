// ZOIC - Arnold camera shader with options for image based bokeh shapes and optical vignetting.

// Special thanks to Marc-Antoine Desjardins for the help on the image sampling
// Special thanks to Benedikt Bitterli for the information on optical vignetting
// Special thanks to Tom Minor for the help with C++ (it was needed!)
// Special thanks to Gaetan Guidet for the C++ cleanup on Github.

// (C) Zeno Pelgrims, www.zenopelgrims.com


// TODO

// Make sure all units are the same (eg kolb is in mm whilst thin lens is in cm..)
// Max aperture is specified in lens description, so clamp when user tries to go wider, also give feedback about this.


#include <ai.h>
#include <cstring>
#include <cstdlib>
#include <iostream>
#include <cmath>
#include <cstdio>
#include <algorithm>
#include <functional>
#include <string>
#include <fstream>
#include <vector>
#include <sstream>
#include <numeric>

#ifdef NO_OIIO
// AiTextureLoad function introduced in arnold 4.2.9.0 was modified in 4.2.10.0
// Figure out the right one to call at compile time
#  if AI_VERSION_ARCH_NUM > 4
#   define AITEXTURELOAD_PROTO2
#  else
#    if AI_VERSION_ARCH_NUM == 4
#      if AI_VERSION_MAJOR_NUM > 2
#        define AITEXTURELOAD_PROTO2
#      else
#        if AI_VERSION_MAJOR_NUM == 2
#          if AI_VERSION_MINOR_NUM >= 10
#            define AITEXTURELOAD_PROTO2
#          endif
#          if AI_VERSION_MINOR_NUM == 9
#            define AITEXTURELOAD_PROTO1
#          endif
#        endif
#      endif
#    endif
#  endif
#  ifdef AITEXTURELOAD_PROTO2
inline bool LoadTexture(const AtString path, void *pixelData){
    return AiTextureLoad(path, true, 0, pixelData);
}
#  else
#    ifdef AITEXTURELOAD_PROTO1
inline bool LoadTexture(const AtString path, void *pixelData){
    return AiTextureLoad(path, true,  pixelData);
}
#    else
inline bool LoadTexture(const AtString path, void *pixelData){
    AiMsgError("Current arnold version doesn't have texture loading API");
    return false;
}
#    endif
#  endif
#else
#  include <OpenImageIO/imageio.h>
#endif

#ifdef _DEBUG
#  define DEBUG_ONLY(block) block
#else
#  define DEBUG_ONLY(block)
#endif

// Arnold thingy
AI_CAMERA_NODE_EXPORT_METHODS(zoicMethods)

#define _sensorWidth  (params[0].FLT)
#define _sensorHeight  (params[1].FLT)
#define _focalLength  (params[2].FLT)
#define _useDof  (params[3].BOOL)
#define _fStop  (params[4].FLT)
#define _focalDistance  (params[5].FLT)
#define _opticalVignettingDistance  (params[6].FLT)
#define _opticalVignettingRadius  (params[7].FLT)
#define _highlightWidth  (params[8].FLT)
#define _highlightStrength  (params[9].FLT)
#define _useImage  (params[10].BOOL)
#define _bokehPath (params[11].STR)
#define _exposureControl (params[12].FLT)
#define _kolb (params[13].BOOL)


struct arrayCompare{
    const float *values;
    inline arrayCompare(const float *_values) :values(_values) {}
    inline bool operator()(int _lhs, int _rhs) const{
        return values[_lhs] > values[_rhs];
    }
};

class imageData{
private:
    int x, y;
    int nchannels;
    float *pixelData;
    float *cdfRow;
    float *cdfColumn;
    int *rowIndices;
    int *columnIndices;

public:
    imageData()
        : x(0), y(0), nchannels(0)
        , pixelData(0), cdfRow(0), cdfColumn(0)
        , rowIndices(0), columnIndices(0) {
    }

    ~imageData(){
        invalidate();
    }

    bool isValid() const{
        return (x * y * nchannels > 0 && nchannels >= 3);
    }

    void invalidate(){
        if (pixelData){
            AiAddMemUsage(-x * y * nchannels * sizeof(float), "zoic");
            AiFree(pixelData);
            pixelData = 0;
        }
        if (cdfRow){
            AiAddMemUsage(-y * sizeof(float), "zoic");
            AiFree(cdfRow);
            cdfRow = 0;
        }
        if (cdfColumn){
            AiAddMemUsage(-x * y * sizeof(float), "zoic");
            AiFree(cdfColumn);
            cdfColumn = 0;
        }
        if (rowIndices){
            AiAddMemUsage(-y * sizeof(int), "zoic");
            AiFree(rowIndices);
            rowIndices = 0;
        }
        if (columnIndices){
            AiAddMemUsage(-x * y * sizeof(int), "zoic");
            AiFree(columnIndices);
            columnIndices = 0;
        }
        x = y = nchannels = 0;
    }

    bool read(const char *bokeh_kernel_filename){

        invalidate();

        AtInt64 nbytes = 0;

#ifdef NO_OIIO

        AiMsgInfo("\x1b[1;36m[ZOIC] Reading image using Arnold API: %s\e[0m", bokeh_kernel_filename);

        AtString path(bokeh_kernel_filename);

        unsigned int iw, ih, nc;
        if (!AiTextureGetResolution(path, &iw, &ih) ||
            !AiTextureGetNumChannels(path, &nc)){
            return false;
        }

        x = int(iw);
        y = int(ih);
        nchannels = int(nc);

        nbytes = x * y * nchannels * sizeof(float);
        AiAddMemUsage(nbytes, "zoic");
        pixelData = (float*) AiMalloc(nbytes);

        if (!LoadTexture(path, pixelData)){
            invalidate();
            return false;
        }

#else

        AiMsgInfo("\x1b[1;36m[ZOIC] Reading image using OpenImageIO: %s\e[0m", bokeh_kernel_filename);

        //Search for an ImageIO plugin that is capable of reading the file ("foo.jpg"), first by
        //trying to deduce the correct plugin from the file extension, but if that fails, by opening
        //every ImageIO plugin it can find until one will open the file without error. When it finds
        //the right plugin, it creates a subclass instance of ImageInput that reads the right kind of
        //file format, and tries to fully open the file.
        OpenImageIO::ImageInput *in = OpenImageIO::ImageInput::open (bokeh_kernel_filename);
        if (! in){
            return false;
        }

        const OpenImageIO::ImageSpec &spec = in->spec();

        x = spec.width;
        y = spec.height;
        nchannels = spec.nchannels;

        nbytes = x * y * nchannels * sizeof(float);
        AiAddMemUsage(nbytes, "zoic");
        pixelData = (float*) AiMalloc(nbytes);

        in->read_image(OpenImageIO::TypeDesc::FLOAT, pixelData);
        in->close();
        delete in;

#endif

        AiMsgInfo("\x1b[1;36m[ZOIC] Image Width: %d\e[0m", x);
        AiMsgInfo("\x1b[1;36m[ZOIC] Image Height: %d\e[0m", y);
        AiMsgInfo("\x1b[1;36m[ZOIC] Image Channels: %d\e[0m", nchannels);
        AiMsgInfo("\x1b[1;36m[ZOIC] Total amount of pixels to process: %d\e[0m", x * y);

        DEBUG_ONLY({
            // print out raw pixel data
            int npixels = x * y;
            for (int i = 0, j = 0; i < npixels; i++){
                std::cout << "[";
                for (int k = 0; k < nchannels; k++, j++){
                    std::cout << pixelData[j];
                    if (k + 1 < nchannels){
                        std::cout << ", ";
                    }
                }
                std::cout << "]";
                if (i + 1 < npixels){
                    std::cout << ", ";
                }
            }
            std::cout << "----------------------------------------------" << std::endl;
            std::cout << "----------------------------------------------" << std::endl;
        })

        bokehProbability();

        return true;
    }

    // Importance sampling
    void bokehProbability(){
        if (!isValid()){
            return;
        }

        // initialize arrays
        AtInt64 nbytes = x * y * sizeof(float);
        AtInt64 totalTempBytes = 0;

        AiAddMemUsage(nbytes, "zoic");
        float *pixelValues = (float*) AiMalloc(nbytes);
        totalTempBytes += nbytes;

        AiAddMemUsage(nbytes, "zoic");
        float *normalizedPixelValues = (float*) AiMalloc(nbytes);
        totalTempBytes += nbytes;

        int npixels = x * y;
        int o1 = (nchannels >= 2 ? 1 : 0);
        int o2 = (nchannels >= 3 ? 2 : o1);
        float totalValue = 0.0f;

        // for every pixel, stuff going wrong here
        for (int i=0, j=0; i < npixels; ++i, j+=nchannels){
            // store pixel value in array
            // calculate luminance [Y = 0.3 R + 0.59 G + 0.11 B]
            pixelValues[i] = pixelData[j] * 0.3f + pixelData[j+o1] * 0.59f + pixelData[j+o2] * 0.11f;

            totalValue += pixelValues[i];

            DEBUG_ONLY(std::cout << "Pixel Luminance: " << i << " -> " << pixelValues[i] << std::endl);
        }

        DEBUG_ONLY({
            std::cout << "----------------------------------------------" << std::endl;
            std::cout << "DEBUG: Total Pixel Value: " << totalValue << std::endl;
            std::cout << "----------------------------------------------" << std::endl;
            std::cout << "----------------------------------------------" << std::endl;
        })

        // normalize pixel values so sum = 1
        float invTotalValue = 1.0f / totalValue;
        float totalNormalizedValue = 0.0f;

        for(int i=0; i < npixels; ++i){
            normalizedPixelValues[i] = pixelValues[i] * invTotalValue;

            totalNormalizedValue += normalizedPixelValues[i];

            DEBUG_ONLY(std::cout << "Normalized Pixel Value: " << i << ": " << normalizedPixelValues[i] << std::endl);
        }

        DEBUG_ONLY({
            std::cout << "----------------------------------------------" << std::endl;
            std::cout << "DEBUG: Total Normalized Pixel Value: " << totalNormalizedValue << std::endl;
            std::cout << "----------------------------------------------" << std::endl;
            std::cout << "----------------------------------------------" << std::endl;
        })

        // calculate sum for each row
        nbytes = y * sizeof(float);
        AiAddMemUsage(nbytes, "zoic");
        float *summedRowValues = (float*) AiMalloc(nbytes);
        totalTempBytes += nbytes;

        for(int i=0, k=0; i < y; ++i){

            summedRowValues[i] = 0.0f;

            for(int j=0; j < x; ++j, ++k){

                summedRowValues[i] += normalizedPixelValues[k];
            }

            DEBUG_ONLY(std::cout << "Summed Values row [" << i << "]: " << summedRowValues[i] << std::endl);
        }


        DEBUG_ONLY({
            // calculate sum of all row values, just to debug
            float totalNormalizedRowValue = 0.0f;
            for(int i=0; i < y; ++i){
                totalNormalizedRowValue += summedRowValues[i];
            }
            std::cout << "----------------------------------------------" << std::endl;
            std::cout << "Debug: Summed Row Value: " << totalNormalizedRowValue << std::endl;
            std::cout << "----------------------------------------------" << std::endl;
        })


        // make array of indices
        nbytes = y * sizeof(int);
        AiAddMemUsage(nbytes, "zoic");
        rowIndices = (int*) AiMalloc(nbytes);

        for(int i = 0; i < y; ++i){
            rowIndices[i] = i;
        }

        std::sort(rowIndices, rowIndices + y, arrayCompare(summedRowValues));


        DEBUG_ONLY({
            // print values
            for(int i = 0; i < y; ++i){
                std::cout << "PDF row [" <<  rowIndices[i] << "]: " << summedRowValues[rowIndices[i]] << std::endl;
            }
            std::cout << "----------------------------------------------" << std::endl;
            std::cout << "----------------------------------------------" << std::endl;
        })


        // For every row, add the sum of all previous row (cumulative distribution function)
        nbytes = y * sizeof(float);
        AiAddMemUsage(nbytes, "zoic");
        cdfRow = (float*) AiMalloc(nbytes);

        float prevVal = 0.0f;

        for (int i = 0; i < y; ++i){
            cdfRow[i] = prevVal + summedRowValues[rowIndices[i]];
            prevVal = cdfRow[i];

            DEBUG_ONLY(std::cout << "CDF row [" << rowIndices[i] << "]: " << cdfRow[i] << std::endl);
        }

        DEBUG_ONLY({
            std::cout << "----------------------------------------------" << std::endl;
            std::cout << "----------------------------------------------" << std::endl;
        })

        nbytes = npixels * sizeof(float);
        AiAddMemUsage(nbytes, "zoic");
        float *normalizedValuesPerRow = (float*) AiMalloc(nbytes);
        totalTempBytes += nbytes;

        // divide pixel values of each pixel by the sum of the pixel values of that row (Normalize)
        for (int r = 0, i = 0; r < y; ++r){
            for (int c = 0; c < x; ++c, ++i){
                // avoid division by 0
                if ((normalizedPixelValues[i] != 0) && (summedRowValues[r] != 0)){
                    normalizedValuesPerRow[i] = normalizedPixelValues[i] / summedRowValues[r];
                }
                else{
                    normalizedValuesPerRow[i] = 0;
                }

                DEBUG_ONLY(std::cout << "Normalized Pixel value per row: " << i << ": " << normalizedValuesPerRow[i] << std::endl);
            }
        }

        DEBUG_ONLY({
            std::cout << "----------------------------------------------" << std::endl;
            std::cout << "----------------------------------------------" << std::endl;
        })

        // sort column values from highest to lowest per row (probability density function)
        nbytes = npixels * sizeof(int);
        AiAddMemUsage(nbytes, "zoic");
        columnIndices = (int*) AiMalloc(nbytes);

        for(int i = 0; i < npixels; i++){
            columnIndices[i] = i;
        }

        for (int i = 0; i < npixels; i+=x){
            std::sort(columnIndices + i, columnIndices + i + x, arrayCompare(normalizedValuesPerRow));
        }

        DEBUG_ONLY({
            // print values
            for(int i = 0; i < npixels; ++i){
                std::cout << "PDF column [" << columnIndices[i] << "]: " << normalizedValuesPerRow[columnIndices[i]] << std::endl;
            }
            std::cout << "----------------------------------------------" << std::endl;
            std::cout << "----------------------------------------------" << std::endl;
        })

        // For every column per row, add the sum of all previous columns (cumulative distribution function)
        nbytes = npixels * sizeof(float);
        AiAddMemUsage(nbytes, "zoic");
        cdfColumn = (float*) AiMalloc(nbytes);

        for (int r = 0, i = 0; r < y; ++r){
            prevVal = 0.0f;

            for (int c = 0; c < x; ++c, ++i){
                cdfColumn[i] = prevVal + normalizedValuesPerRow[columnIndices[i]];
                prevVal = cdfColumn[i];

                DEBUG_ONLY(std::cout << "CDF column [" <<  columnIndices[i] << "]: " << cdfColumn[i] << std::endl);
            }
        }

        DEBUG_ONLY(std::cout << "----------------------------------------------" << std::endl);

        // Release and untrack memory
        AiAddMemUsage(-totalTempBytes, "zoic");

        AiFree(pixelValues);
        AiFree(normalizedPixelValues);
        AiFree(summedRowValues);
        AiFree(normalizedValuesPerRow);
    }

    // Sample image
    void bokehSample(float randomNumberRow, float randomNumberColumn, float *dx, float *dy){

        if (!isValid()){
            AiMsgWarning("[ZOIC] Invalid bokeh image data.");
            *dx = 0.0f;
            *dy = 0.0f;
            return;
        }

        // print random number between 0 and 1
        DEBUG_ONLY(std::cout << "RANDOM NUMBER ROW: " << randomNumberRow << std::endl);

        // find upper bound of random number in the array
        float *pUpperBound = std::upper_bound(cdfRow, cdfRow + y, randomNumberRow);
        int r = 0;

        if (pUpperBound >= cdfRow + y){
            //AiMsgWarning("[zoic] %f larger than last biggest cdfRow[%d] = %f", randomNumberRow, y-1, cdfRow[y-1]);
            r = y - 1;

        } else{
            DEBUG_ONLY(std::cout << "UPPER BOUND: " << *pUpperBound << std::endl);
            r = int(pUpperBound - cdfRow);
        }


        // find actual pixel row
        int actualPixelRow = rowIndices[r];

        // recalculate pixel row so that the center pixel is (0,0) - might run into problems with images of dimensions like 2x2, 4x4, 6x6, etc
        int recalulatedPixelRow = actualPixelRow - ((x - 1) / 2);

        DEBUG_ONLY({
            // print values
            std::cout << "INDEX IN CDF ROW: " << r << std::endl;
            std::cout << "ACTUAL PIXEL ROW: " << actualPixelRow << std::endl;
            std::cout << "RECALCULATED PIXEL ROW: " << recalulatedPixelRow << std::endl;
            std::cout << "----------------------------------------------" << std::endl;
            std::cout << "----------------------------------------------" << std::endl;
            // print random number between 0 and 1
            std::cout << "RANDOM NUMBER COLUMN: " << randomNumberColumn << std::endl;
        })

        int startPixel = actualPixelRow * x;

        DEBUG_ONLY(std::cout << "START PIXEL: " << startPixel << std::endl);


        // find upper bound of random number in the array
        float *pUpperBoundColumn = std::upper_bound(cdfColumn + startPixel, cdfColumn + startPixel + x, randomNumberColumn);
        int c = 0;

        if (pUpperBoundColumn >= cdfColumn + startPixel + x){
            //AiMsgWarning("[zoic] %f larger than last biggest cdfColumn[%d][%d] = %f", randomNumberColumn, r, x-1, cdfColumn[startPixel+x-1]);
            c = startPixel + x - 1;

        } else{
            DEBUG_ONLY(std::cout << "UPPER BOUND: " << *pUpperBoundColumn << std::endl);
            c = int(pUpperBoundColumn - cdfColumn);
        }

        // find actual pixel column
        int actualPixelColumn = columnIndices[c];
        int relativePixelColumn = actualPixelColumn - startPixel;
        int recalulatedPixelColumn = relativePixelColumn - ((y - 1) / 2);

        DEBUG_ONLY({
            // print values
            std::cout << "INDEX IN CDF COLUMN: " << c << std::endl;
            std::cout << "ACTUAL PIXEL COLUMN: " << actualPixelColumn << std::endl;
            std::cout << "RELATIVE PIXEL COLUMN (starting from 0): " << relativePixelColumn << std::endl;
            std::cout << "RECALCULATED PIXEL COLUMN: " << recalulatedPixelColumn << std::endl;
            std::cout << "----------------------------------------------" << std::endl;
            std::cout << "----------------------------------------------" << std::endl;
        })

        // to get the right image orientation, flip the x and y coordinates and then multiply the y values by -1 to flip the pixels vertically
        float flippedRow = float(recalulatedPixelColumn);
        float flippedColumn = recalulatedPixelRow * -1.0f;

        // send values back
        *dx = (float)flippedRow / (float)x * 2;
        *dy = (float)flippedColumn / (float)y * 2;
    }
};

struct cameraData{
    float fov;
    float tan_fov;
    float apertureRadius;
    imageData image;

    cameraData()
        : fov(0.0f), tan_fov(0.0f), apertureRadius(0.0f){
    }

    ~cameraData(){
    }
};



// PBRT v2 source code  - Concentric disk sampling (Sampling the disk in a more uniform way than with random sampling)
inline void ConcentricSampleDisk(float u1, float u2, float *dx, float *dy) {
    float radius; // radius
    float theta; // angle

    // Map uniform random numbers to $[-1,1]^2$
    float sx = 2.0f * u1 - 1.0f;
    float sy = 2.0f * u2 - 1.0f;

    // Map square to $(r,\theta)$
    // Handle degeneracy at the origin
    if (sx == 0.0f && sy == 0.0f){
        *dx=0.0f;
        *dy = 0.0f;
    }
    if (sx >= -sy) {
        if (sx > sy) {
            // Handle first region of disk
            radius = sx;
            if (sy > 0.0f) theta = sy/radius;
            else          theta = 8.0f + sy/radius;
        }
        else {
            // Handle second region of disk
            radius = sy;
            theta = 2.0f - sx/radius;
        }
    }
    else {
        if (sx <= sy) {
            // Handle third region of disk
            radius = -sx;
            theta = 4.0f - sy/radius;
        }
        else {
            // Handle fourth region of disk
            radius = -sy;
            theta = 6.0f + sx/radius;
        }
    }

    theta *= AI_PI / 4.0f;
    *dx = radius * std::cos(theta);
    *dy = radius * std::sin(theta);
}



struct Lensdata{
    std::vector<double> lensRadiusCurvature;
    std::vector<double> lensThickness;
    std::vector<double> lensIOR;
    std::vector<double> lensAperture;
    double apertureDistance;
    int vignettedRays, succesRays;
    float xres, yres;
} ld;



// READ IN TABULAR LENS DATA
void readTabularLensData(std::string lensDataFileName, Lensdata *ld){

    // reset vectors
    ld->lensAperture.clear();
    ld->lensIOR.clear();
    ld->lensRadiusCurvature.clear();
    ld->lensAperture.clear();

    std::ifstream lensDataFile(lensDataFileName);
    std::string line;
    std::string token;
    std::stringstream iss;
    int lensDataCounter = 1;


    AiMsgInfo("\x1b[1;36m[ZOIC] ##############################################\e[0m");
    AiMsgInfo("\x1b[1;36m[ZOIC] ############# READING LENS DATA ##############\e[0m");
    AiMsgInfo("\x1b[1;36m[ZOIC] ##############################################\e[0m");
    AiMsgInfo("\x1b[1;36m[ZOIC] If you're reading this, welcome to the nerd club :-D\e[0m");

    while (getline(lensDataFile, line))
    {
        if (line.length() == 0 || line[0] == '#'){
            AiMsgInfo("\x1b[1;36m[ZOIC] Comment or empty line, skipping line\e[0m");
        }
        else {
            iss << line;

            // put values (converting from string to float) into the vectors
            while (getline(iss, token, '\t') ){
                if (token == " "){
                   AiMsgError("[ZOIC] Please make sure your .dat file only contains TAB spacings.");
                }

                if (lensDataCounter == 1){
                    ld->lensRadiusCurvature.push_back (std::stod(token));
                }

                if (lensDataCounter == 2){
                    ld->lensThickness.push_back (std::stod(token));
                }

                if (lensDataCounter == 3){
                    // check for 0.0 IOR case, set to 1.0
                    double number = std::stod(token);
                    if(number == 0.0){
                        number = 1.0;
                    }
                    ld->lensIOR.push_back (number);
                }

                if (lensDataCounter == 4){
                    ld->lensAperture.push_back (std::stod(token));
                    lensDataCounter = 0;
                }
                lensDataCounter += 1;
            }

            iss.clear();
        }
    }

    AiMsgInfo("\x1b[1;36m[ZOIC] ##############################################\e[0m");
    AiMsgInfo("\x1b[1;36m[ZOIC] # ROC \t Thickness \t IOR \t Aperture #\e[0m");
    AiMsgInfo("\x1b[1;36m[ZOIC] ##############################################\e[0m");

    for(int i = 0; i < ld->lensRadiusCurvature.size(); i++){
        AiMsgInfo("\x1b[1;36m[ZOIC] %f    %f    %f    %f\e[0m", ld->lensRadiusCurvature[i], ld->lensThickness[i], ld->lensIOR[i], ld->lensAperture[i]);
    }

    AiMsgInfo("\x1b[1;36m[ZOIC] ##############################################\e[0m");
    AiMsgInfo("\x1b[1;36m[ZOIC] ########### END READING LENS DATA ############\e[0m");
    AiMsgInfo("\x1b[1;36m[ZOIC] ##############################################\e[0m");

    // reverse the datasets in the vector, since we will start with the rear-most lens element
    std::reverse(ld->lensRadiusCurvature.begin(),ld->lensRadiusCurvature.end());
    std::reverse(ld->lensThickness.begin(),ld->lensThickness.end());
    std::reverse(ld->lensIOR.begin(),ld->lensIOR.end());
    std::reverse(ld->lensAperture.begin(),ld->lensAperture.end());

}


// RAY SPHERE INTERSECTIONS
AtVector raySphereIntersection(AtVector ray_direction, AtVector ray_origin, AtVector sphere_center, double sphere_radius, bool reverse){

    ray_direction = AiV3Normalize(ray_direction);
    AtVector L = sphere_center - ray_origin;

    // project the directionvector onto the distancevector
    double tca = AiV3Dot(L, ray_direction);

    double radius2 = sphere_radius * sphere_radius;

    // if intersection is in the opposite direction of the ray, don't worry about it
    //if (tca < 0.0) {return vec3(0.0,0.0,0.0);}

    double d2 = AiV3Dot(L, L) - tca * tca;

    // if the distance from the ray to the spherecenter is larger than its radius, don't worry about it
    // come up with a better way of killing the ray (already there in path tracer)
    //if (d2 > radius2){return vec3(0.0,0.0,0.0);}

    // pythagoras' theorem
    double thc = sqrt(radius2 - d2);

    if(!reverse){
        return ray_origin + ray_direction * (tca + thc * std::copysign(1.0, sphere_radius));
    }
    else{
        return ray_origin + ray_direction * (tca - thc * copysign(1.0, sphere_radius));
    }
}



// COMPUTE NORMAL HITPOINT
AtVector intersectionNormal(AtVector hit_point, AtVector sphere_center, double sphere_radius){
    return AiV3Normalize(sphere_center - hit_point) * std::copysign(1.0, sphere_radius);
}



// TRANSMISSION VECTOR
AtVector calculateTransmissionVector(double ior1, double ior2, AtVector incidentVector, AtVector normalVector){

    AtVector transmissionVector;

    // VECTORS NEED TO BE NORMALIZED BEFORE USE!
    incidentVector = AiV3Normalize(incidentVector);
    normalVector = AiV3Normalize(normalVector);


    double eta = ior1 / ior2;
    double c1 = - AiV3Dot(incidentVector, normalVector);
    double cs2 = eta * eta * (1.0 - c1 * c1);

    //if (cs2 > 1.0){ // total internal reflection, can only occur when ior1 > ior2
        //std::cout << "Total internal reflection case";
        // kill ray here
    //}

    double cosT = sqrt(std::abs(1.0 - cs2));

    transmissionVector = (incidentVector * eta) + (normalVector * ((eta * c1) - cosT));

    return transmissionVector;

}



// LINE LINE INTERSECTIONS
AtVector2 lineLineIntersection(AtVector line1_origin, AtVector line1_direction, AtVector line2_origin, AtVector line2_direction){
    // Get A,B,C of first line - points : ps1 to pe1
    double A1 = line1_direction.y - line1_origin.y;
    double B1 = line1_origin.x - line1_direction.x;
    double C1 = A1 * line1_origin.x + B1 * line1_origin.y;

    // Get A,B,C of second line - points : ps2 to pe2
    double A2 = line2_direction.y - line2_origin.y;
    double B2 = line2_origin.x - line2_direction.x;
    double C2 = A2 * line2_origin.x + B2 * line2_origin.y;

    // Get delta and check if the lines are parallel
    double delta = A1 * B2 - A2 * B1;

    // now return the Vector2 intersection point
    AtVector2 intersectionPoint;
    intersectionPoint.x = (B2 * C1 - B1 * C2) / delta;
    intersectionPoint.y = (A1 * C2 - A2 * C1) / delta;

    return intersectionPoint;
}


// CALCULATE IMAGE DISTANCE
double calculateImageDistance(double objectDistance, Lensdata *ld){

    double imageDistance;
    AtVector ray_origin_focus;
    ray_origin_focus.x = objectDistance;
    ray_origin_focus.y = 0.0;
    ray_origin_focus.z = 0.0;


    // 20.0 needs to be changed to a number as small as possible whilst still getting no numerical errors. (eg 0.001)
    AtVector ray_direction_focus;
    ray_direction_focus.x = - objectDistance;
    ray_direction_focus.y = 20.0;
    ray_direction_focus.z = 0.0;

    double summedThickness_focus = 0.0;

    for(int i = 0; i < ld->lensRadiusCurvature.size(); i++){
        if(i==0){
            for(int k = 0; k < ld->lensRadiusCurvature.size(); k++){
                summedThickness_focus += ld->lensThickness[k];
            }
        }

        // (condition) ? true : false;
        i == 0 ? summedThickness_focus = summedThickness_focus : summedThickness_focus -= ld->lensThickness[ld->lensRadiusCurvature.size() - i];

        if(ld->lensRadiusCurvature[i] == 0.0){
            ld->lensRadiusCurvature[i] = 99999.0;
        }

        AtVector sphere_center;
        sphere_center.x = summedThickness_focus - ld->lensRadiusCurvature[ld->lensRadiusCurvature.size() -1 -i];
        sphere_center.y = 0.0;
        sphere_center.z = 0.0;

        AtVector hit_point;
        hit_point = raySphereIntersection(ray_direction_focus, ray_origin_focus, sphere_center, ld->lensRadiusCurvature[ld->lensRadiusCurvature.size() - 1 - i], true);

        AtVector hit_point_normal;
        hit_point_normal = intersectionNormal(hit_point, sphere_center, - ld->lensRadiusCurvature[ld->lensRadiusCurvature.size() - 1 - i]);


        if(i==0){
            ray_direction_focus = calculateTransmissionVector(1.0, ld->lensIOR[ld->lensRadiusCurvature.size() - 1 - i], ray_direction_focus, hit_point_normal);
        }
        else{
            ray_direction_focus = calculateTransmissionVector(ld->lensIOR[ld->lensRadiusCurvature.size() - i], ld->lensIOR[ld->lensRadiusCurvature.size() - i - 1], ray_direction_focus, hit_point_normal);
        }

        // set hitpoint to be the new origin
        ray_origin_focus = hit_point;

        // shoot off rays after last refraction
        if(i == ld->lensRadiusCurvature.size() - 1){
            ray_direction_focus = calculateTransmissionVector(ld->lensIOR[ld->lensRadiusCurvature.size() - 1 - i], 1.0, ray_direction_focus, hit_point_normal);

            // find intersection point
            AtVector axialStart;
            axialStart.x = -99999.0;
            axialStart.y = 0.0;
            axialStart.z = 0.0;

            AtVector axialEnd;
            axialStart.x = 99999.0;
            axialStart.y = 0.0;
            axialStart.z = 0.0;

            AtVector lineDirection;
            lineDirection.x = ray_origin_focus.x + ray_direction_focus.x;
            lineDirection.y = ray_origin_focus.y + ray_direction_focus.y;
            lineDirection.z = 0.0;

            imageDistance = lineLineIntersection(axialStart, axialEnd, ray_origin_focus, lineDirection).x;
        }

    }

    AiMsgInfo("\x1b[1;36m[ZOIC] Object distance = [%f]\e[0m", objectDistance);
    AiMsgInfo("\x1b[1;36m[ZOIC] Image distance = [%f]\e[0m", imageDistance);
    return imageDistance;
}



void traceThroughLensElements(AtVector *ray_origin, AtVector *ray_direction, float *weight, Lensdata *ld, float lensx, float lensy){

    AtVector hit_point;
    AtVector hit_point_normal;
    AtVector sphere_center;
    double summedThickness;
    // obviously change this to a variable
    double apertureRadius = 20.0;
    bool lensElementAperture;

    for(int i = 0; i < ld->lensRadiusCurvature.size(); i++){

        // (condition) ? true : false;
        i == 0 ? summedThickness = ld->lensThickness[0] : summedThickness += ld->lensThickness[i];

        if(ld->lensRadiusCurvature[i] == 0.0 || ld->lensRadiusCurvature[i] == 99999.0){
            ld->lensRadiusCurvature[i] = 99999.0;
            lensElementAperture = true;
        }

        sphere_center.x = 0.0;
        sphere_center.y = 0.0;
        sphere_center.z = summedThickness - ld->lensRadiusCurvature[i];

        hit_point = raySphereIntersection(*ray_direction, *ray_origin, sphere_center, ld->lensRadiusCurvature[i], false);

        // set hitpoint to be the new origin
        *ray_origin = hit_point;

        double hitPointHypotenuse = sqrt(hit_point.x * hit_point.x + hit_point.y * hit_point.y);


        if(hitPointHypotenuse > (ld->lensAperture[i]/2.0)){
            ld->vignettedRays += 1;
            *weight = 0.0f;
        }

        if(lensElementAperture == true && hitPointHypotenuse > apertureRadius){
            ld->vignettedRays += 1;
            *weight = 0.0f;
        }

        hit_point_normal = intersectionNormal(hit_point, sphere_center, ld->lensRadiusCurvature[i]);

        AtVector tmpRayDirection;

        // if ior1 and ior2 are not the same, calculate new ray direction vector
        if(ld->lensIOR[i] != ld->lensIOR[i+1]){
            tmpRayDirection = calculateTransmissionVector(ld->lensIOR[i], ld->lensIOR[i+1], *ray_direction, hit_point_normal);
        }

        // shoot off rays after last refraction
        if(i == ld->lensRadiusCurvature.size() - 1){ // last element in array
            tmpRayDirection = calculateTransmissionVector(ld->lensIOR[i], 1.0, *ray_direction, hit_point_normal);
        }

        *ray_direction = tmpRayDirection;
        ld->succesRays += 1;
    }
}



node_parameters {
    AiParameterFLT("sensorWidth", 3.6f); // 35mm film
    AiParameterFLT("sensorHeight", 2.4f); // 35 mm film
    AiParameterFLT("focalLength", 65.0f); // distance between sensor and lens
    AiParameterBOOL("useDof", true);
    AiParameterFLT("fStop", 1.4f);
    AiParameterFLT("focalDistance", 110.0f); // distance from lens to focal point
    AiParameterFLT("opticalVignettingDistance", 0.0f); //distance of the opticalVignetting virtual aperture
    AiParameterFLT("opticalVignettingRadius", 0.0f); // 1.0 - .. range float, to multiply with the actual aperture radius
    AiParameterFLT("highlightWidth", 0.2f);
    AiParameterFLT("highlightStrength", 10.0f);
    AiParameterBOOL("useImage", false);
    AiParameterStr("bokehPath", ""); //bokeh shape image location
    AiParameterFLT("exposureControl", 0.0f);
    AiParameterBOOL("kolb", true);
}


node_initialize {
    cameraData *camera = new cameraData();
    AiCameraInitialize(node, (void*)camera);

}

node_update {
    AiCameraUpdate(node, false);

    cameraData *camera = (cameraData*) AiCameraGetLocalData(node);

    // calculate field of view (theta = 2arctan*(sensorSize/focalLength))
    camera->fov = 2.0f * atan((_sensorWidth / (2.0f * (_focalLength/10)))); // in radians
    camera->tan_fov = tanf(camera->fov/ 2);

    // calculate aperture radius (apertureRadius = focalLength / 2*fStop)
    camera->apertureRadius = (_focalLength/10) / (2*_fStop);

    camera->image.invalidate();

    // make probability functions of the bokeh image
    if (_useImage == true){
       if (!camera->image.read(_bokehPath)){
            AiMsgError("[ZOIC] Couldn't open image!");
            AiRenderAbort();
       }
    }

    if (_kolb){
        // Update shaderData variables
        AtNode* options = AiUniverseGetOptions();
        ld.xres = static_cast<float>(AiNodeGetInt(options,"xres"));
        ld.yres = static_cast<float>(AiNodeGetInt(options,"yres"));
        AiMsgInfo("\x1b[1;36m[ZOIC] xres, yres = [%f, %f]\e[0m", ld.xres, ld.yres);


        // read in lens data file
        std::string lensDataFileName;
        lensDataFileName = "/Users/zpelgrims/Downloads/lens/dgauss.100mm.dat";
        readTabularLensData(lensDataFileName, &ld);

        // change number for variable
        // shift first lens element (and all others consequently) so that
        // the image distance at a certain object distance falls on the film plane
        ld.lensThickness[0] -= calculateImageDistance(3000.0, &ld);

        // find how far the aperture is from the film plane
        ld.apertureDistance = 0.0;
        for(int i = 0; i < ld.lensRadiusCurvature.size(); i++){
            ld.apertureDistance += ld.lensThickness[i];
            if(ld.lensRadiusCurvature[i] == 0.0 || ld.lensRadiusCurvature[i] == 99999.0){
                AiMsgInfo("\x1b[1;36m[ZOIC] Aperture distance after lens shift = [%f]\e[0m", ld.apertureDistance);
                break;
            }
        }
    }

}

node_finish {
    cameraData *camera = (cameraData*) AiCameraGetLocalData(node);

    AiMsgInfo("\x1b[1;36m[ZOIC] Succesful rays = [%d]\e[0m", ld.succesRays);
    AiMsgInfo("\x1b[1;36m[ZOIC] Vignetted rays = [%d]\e[0m", ld.vignettedRays);
    AiMsgInfo("\x1b[1;36m[ZOIC] Succes Percentage = [%f]\e[0m", (float(ld.vignettedRays) / float(ld.succesRays) * 100.0));

    delete camera;
    AiCameraDestroy(node);
}


camera_create_ray {

    // get values
    const AtParamValue* params = AiNodeGetParams(node);
    cameraData *camera = (cameraData*) AiCameraGetLocalData(node);


    // chang this to an enum, thinlens, raytraced
    if(!_kolb){

        // create point on lens
        AtPoint p;
        p.x = input->sx * camera->tan_fov;
        p.y = input->sy * camera->tan_fov;
        p.z = 1.0;

        // compute direction
        output->dir = AiV3Normalize(p - output->origin);

        // now looking down -Z
        output->dir.z *= -1.0;

        // DOF CALCULATIONS
        bool tmpUseDof = false;
        //if (_useDof == true) {
        if (tmpUseDof == true) {

            // Initialize point on lens
            float lensU = 0.0f;
            float lensV = 0.0f;

            // sample disk with proper sample distribution, lensU & lensV (positions on lens) are updated.
            if (_useImage == false){
                ConcentricSampleDisk(input->lensx, input->lensy, &lensU, &lensV);
            } else {
                // sample bokeh image
                camera->image.bokehSample(input->lensx, input->lensy, &lensU, &lensV);
            }

            // scale new lens coordinates by the aperture radius
            lensU = lensU * camera->apertureRadius;
            lensV = lensV * camera->apertureRadius;

            // update arnold ray origin
            output->origin.x = lensU;
            output->origin.y = lensV;
            output->origin.z = 0.0;

            // Compute point on plane of focus, intersection on z axis
            float intersection = std::abs(_focalDistance / output->dir.z);
            AtPoint focusPoint = output->dir * intersection;

            // update arnold ray direction, normalize
            output->dir = AiV3Normalize(focusPoint - output->origin);

            // Optical Vignetting (CAT EYE EFFECT)
            if (_opticalVignettingDistance > 0.0f){
                // because the first intersection point of the aperture is already known, I can just linearly scale it by the distance to the second aperture
                AtPoint opticalVignetPoint;
                opticalVignetPoint = output->dir * _opticalVignettingDistance;

                // re-center point
                opticalVignetPoint -= output->origin;

                // find hypotenuse of x, y points.
                float pointHypotenuse = sqrt((opticalVignetPoint.x * opticalVignetPoint.x) + (opticalVignetPoint.y * opticalVignetPoint.y));

                // if intersection point on the optical vignetting virtual aperture is within the radius of the aperture from the plane origin, kill ray
                float virtualApertureTrueRadius = camera->apertureRadius * _opticalVignettingRadius;

                // set ray weight to 0, there is an optimisation inside Arnold that doesn't send rays if they will return black anyway.
                if (ABS(pointHypotenuse) > virtualApertureTrueRadius){output->weight = 0.0f;}

                // inner highlight,if point is within domain between lens radius and new inner radius (defined by the width)
                // adding weight to opposite edges to get nice rim on the highlights
                else if (ABS(pointHypotenuse) < virtualApertureTrueRadius && ABS(pointHypotenuse) > (virtualApertureTrueRadius - _highlightWidth)){
                    output->weight *= _highlightStrength * (1 - (virtualApertureTrueRadius - ABS(pointHypotenuse))) * sqrt(input->sx * input->sx + input->sy * input->sy);
                }
            }
        }
    }

    // only working half arsed right now, many things might be wrong.
    if(_kolb){

        output->origin.x = (input->sx) * 36.0;
        output->origin.y = (input->sy) * 24.0;
        output->origin.z = 0.0;



        /*

        AtPoint2 s = {input->sx / ld.xres, input->sy / ld.yres};

        float aspect = ld.xres / ld.yres;

        float diagonal = sqrt(_sensorWidth * _sensorWidth + _sensorHeight *  _sensorHeight);

        float x = sqrt(diagonal * diagonal / (1.0 + aspect * aspect));
        float y = aspect * x;

        AtPoint2 notsure1 = {float(-x / 2.0), float(-y / 2.0)};
        AtPoint2 notsure2 = {float(x / 2.0), float(y / 2.0)};

        // not sure about this whole lerp thing, how do i go about using this stuff?
        AtPoint2 pFilm2;
        pFilm2.x = AiV2Lerp(s.x, notsure1, notsure2);
        pFilm2.y = AiV2Lerp(s.y, notsure1, notsure2);

        output->origin = {-pFilm2.x, pFilm2.y, 0.0};

        */



        // determine in which direction to shoot the rays
        float lensU, lensV = 0.0;
        ConcentricSampleDisk(input->lensx, input->lensy, &lensU, &lensV);

        output->dir.x = lensU;// * 10.0; //change to proper aperture radius
        output->dir.y = lensV;// * 10.0;
        output->dir.z = ld.apertureDistance;

        //output->dir.y *= -1.0;
        //output->dir = AiV3Normalize(output->dir - output->origin);

        traceThroughLensElements(&output->origin, &output->dir, &output->weight, &ld, input->lensx, input->lensy);

        // flip ray direction
        output->dir *= -1.0;
    }

    // control to go light stops up and down
    float e2 = _exposureControl * _exposureControl;
    if (_exposureControl > 0){
        output->weight *= 1.0f + e2;
    }
    else if (_exposureControl < 0){
        output->weight *= 1.0f / (1.0f + e2);
    }
}


node_loader {
   if (i > 0)
      return false;
   node->methods      = zoicMethods;
   node->output_type  = AI_TYPE_NONE;
   node->name         = "zoic";
   node->node_type    = AI_NODE_CAMERA;
   strcpy(node->version, AI_VERSION);
   return true;
}
