/******************************************************************************
 * $Id$
 *
 * Project:  Virtual GDAL Datasets
 * Purpose:  Implementation of some filter types.
 * Author:   Frank Warmerdam <warmerdam@pobox.com>
 *
 ******************************************************************************
 * Copyright (c) 2003, Frank Warmerdam <warmerdam@pobox.com>
 * Copyright (c) 2008-2013, Even Rouault <even dot rouault at mines-paris dot org>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 ****************************************************************************/

#include "vrtdataset.h"
#include "cpl_minixml.h"
#include "cpl_string.h"

CPL_CVSID("$Id$");

/************************************************************************/
/* ==================================================================== */
/*                          VRTFilteredSource                           */
/* ==================================================================== */
/************************************************************************/

/************************************************************************/
/*                         VRTFilteredSource()                          */
/************************************************************************/

VRTFilteredSource::VRTFilteredSource() :
    nSupportedTypesCount(1),
    nExtraEdgePixels(0)
{
    aeSupportedTypes[0] = GDT_Float32;
}

/************************************************************************/
/*                         ~VRTFilteredSource()                         */
/************************************************************************/

VRTFilteredSource::~VRTFilteredSource() {}

/************************************************************************/
/*                         SetExtraEdgePixels()                         */
/************************************************************************/

void VRTFilteredSource::SetExtraEdgePixels( int nEdgePixels )

{
    nExtraEdgePixels = nEdgePixels;
}

/************************************************************************/
/*                   SetFilteringDataTypesSupported()                   */
/************************************************************************/

void VRTFilteredSource::SetFilteringDataTypesSupported( int nTypeCount, 
                                                        GDALDataType *paeTypes)

{
    if( nTypeCount >
        static_cast<int>( sizeof(aeSupportedTypes) / sizeof(GDALDataType) ) )
    {
        CPLAssert( FALSE );
        nTypeCount = static_cast<int>(
            sizeof(aeSupportedTypes) / sizeof(GDALDataType) );
    }

    nSupportedTypesCount = nTypeCount;
    memcpy( aeSupportedTypes, paeTypes, sizeof(GDALDataType) * nTypeCount );
}

/************************************************************************/
/*                          IsTypeSupported()                           */
/************************************************************************/

int VRTFilteredSource::IsTypeSupported( GDALDataType eTestType )

{
    for( int i = 0; i < nSupportedTypesCount; i++ )
    {
        if( eTestType == aeSupportedTypes[i] )
            return TRUE;
    }

    return FALSE;
}

/************************************************************************/
/*                              RasterIO()                              */
/************************************************************************/

CPLErr
VRTFilteredSource::RasterIO( int nXOff, int nYOff, int nXSize, int nYSize, 
                             void *pData, int nBufXSize, int nBufYSize, 
                             GDALDataType eBufType, 
                             GSpacing nPixelSpace,
                             GSpacing nLineSpace,
                             GDALRasterIOExtraArg* psExtraArg )

{
/* -------------------------------------------------------------------- */
/*      For now we don't support filtered access to non-full            */
/*      resolution requests. Just collect the data directly without     */
/*      any operator.                                                   */
/* -------------------------------------------------------------------- */
    if( nBufXSize != nXSize || nBufYSize != nYSize )
    {
        return VRTComplexSource::RasterIO( nXOff, nYOff, nXSize, nYSize, 
                                           pData, nBufXSize, nBufYSize, 
                                           eBufType, nPixelSpace, nLineSpace, psExtraArg );
    }

    // The window we will actually request from the source raster band.
    double dfReqXOff, dfReqYOff, dfReqXSize, dfReqYSize;
    int nReqXOff, nReqYOff, nReqXSize, nReqYSize;

    // The window we will actual set _within_ the pData buffer.
    int nOutXOff, nOutYOff, nOutXSize, nOutYSize;

    if( !GetSrcDstWindow( nXOff, nYOff, nXSize, nYSize, nBufXSize, nBufYSize,
                        &dfReqXOff, &dfReqYOff, &dfReqXSize, &dfReqYSize,
                        &nReqXOff, &nReqYOff, &nReqXSize, &nReqYSize,
                        &nOutXOff, &nOutYOff, &nOutXSize, &nOutYSize ) )
        return CE_None;

    pData = reinterpret_cast<GByte *>( pData )
        + nPixelSpace * nOutXOff
        + nLineSpace * nOutYOff;

/* -------------------------------------------------------------------- */
/*      Determine the data type we want to request.  We try to match    */
/*      the source or destination request, and if both those fail we    */
/*      fallback to the first supported type at least as expressive     */
/*      as the request.                                                 */
/* -------------------------------------------------------------------- */
    GDALDataType eOperDataType = GDT_Unknown;

    if( IsTypeSupported( eBufType ) )
        eOperDataType = eBufType;

    if( eOperDataType == GDT_Unknown 
        && IsTypeSupported( poRasterBand->GetRasterDataType() ) )
        eOperDataType = poRasterBand->GetRasterDataType();

    if( eOperDataType == GDT_Unknown )
    {
        for( int i = 0; i < nSupportedTypesCount; i++ )
        {
            if( GDALDataTypeUnion( aeSupportedTypes[i], eBufType ) 
                == aeSupportedTypes[i] )
            {
                eOperDataType = aeSupportedTypes[i];
            }
        }
    }

    if( eOperDataType == GDT_Unknown )
    {
        eOperDataType = aeSupportedTypes[0];

        for( int i = 1; i < nSupportedTypesCount; i++ )
        {
            if( GDALGetDataTypeSize( aeSupportedTypes[i] ) 
                > GDALGetDataTypeSize( eOperDataType ) )
            {
                eOperDataType = aeSupportedTypes[i];
            }
        }
    }

/* -------------------------------------------------------------------- */
/*      Allocate the buffer of data into which our imagery will be      */
/*      read, with the extra edge pixels as well. This will be the      */
/*      source data fed into the filter.                                */
/* -------------------------------------------------------------------- */
    int nPixelOffset, nLineOffset;
    int nExtraXSize = nOutXSize + 2 * nExtraEdgePixels;
    int nExtraYSize = nOutYSize + 2 * nExtraEdgePixels;

    // FIXME? : risk of multiplication overflow
    GByte *pabyWorkData = reinterpret_cast<GByte *>(
        VSICalloc( nExtraXSize * nExtraYSize,
                   GDALGetDataTypeSize(eOperDataType) / 8 ) );

    if( pabyWorkData == NULL )
    {
        CPLError( CE_Failure, CPLE_OutOfMemory, 
                  "Work buffer allocation failed." );
        return CE_Failure;
    }

    nPixelOffset = GDALGetDataTypeSize( eOperDataType ) / 8;
    nLineOffset = nPixelOffset * nExtraXSize;

/* -------------------------------------------------------------------- */
/*      Allocate the output buffer if the passed in output buffer is    */
/*      not of the same type as our working format, or if the passed    */
/*      in buffer has an unusual organization.                          */
/* -------------------------------------------------------------------- */
    GByte *pabyOutData = NULL;

    if( nPixelSpace != nPixelOffset || nLineSpace != nLineOffset
        || eOperDataType != eBufType )
    {
        pabyOutData = reinterpret_cast<GByte *>(
            VSIMalloc3( nOutXSize, nOutYSize, nPixelOffset ) );

        if( pabyOutData == NULL )
        {
            CPLError( CE_Failure, CPLE_OutOfMemory,
                      "Work buffer allocation failed." );
            CPLFree( pabyWorkData );
            return CE_Failure;
        }
    }
    else
        pabyOutData = reinterpret_cast<GByte *>( pData );

/* -------------------------------------------------------------------- */
/*      Figure out the extended window that we want to load.  Note      */
/*      that we keep track of the file window as well as the amount     */
/*      we will need to edge fill past the edge of the source dataset.  */
/* -------------------------------------------------------------------- */
    int nFileXOff = nReqXOff - nExtraEdgePixels;
    int nFileYOff = nReqYOff - nExtraEdgePixels;
    int nFileXSize = nExtraXSize;
    int nFileYSize = nExtraYSize;

    int nTopFill=0;
    int nLeftFill=0;
    int nRightFill=0;
    int nBottomFill=0;

    if( nFileXOff < 0 )
    {
        nLeftFill = -nFileXOff;
        nFileXOff = 0;
        nFileXSize -= nLeftFill;
    }

    if( nFileYOff < 0 )
    {
        nTopFill = -nFileYOff;
        nFileYOff = 0;
        nFileYSize -= nTopFill;
    }

    if( nFileXOff + nFileXSize > poRasterBand->GetXSize() )
    {
        nRightFill = nFileXOff + nFileXSize - poRasterBand->GetXSize();
        nFileXSize -= nRightFill;
    }

    if( nFileYOff + nFileYSize > poRasterBand->GetYSize() )
    {
        nBottomFill = nFileYOff + nFileYSize - poRasterBand->GetYSize();
        nFileYSize -= nBottomFill;
    }

/* -------------------------------------------------------------------- */
/*      Load the data.                                                  */
/* -------------------------------------------------------------------- */
    CPLErr eErr
        = VRTComplexSource::RasterIOInternal(
            nFileXOff, nFileYOff, nFileXSize, nFileYSize,
            pabyWorkData + nLineOffset * nTopFill + nPixelOffset * nLeftFill,
            nFileXSize, nFileYSize, eOperDataType,
            nPixelOffset, nLineOffset, psExtraArg );

    if( eErr != CE_None )
    {
        if( pabyWorkData != pData )
            VSIFree( pabyWorkData );

        if( pabyOutData != pData )
            VSIFree( pabyOutData );

        return eErr;
    }

/* -------------------------------------------------------------------- */
/*      Fill in missing areas.  Note that we replicate the edge         */
/*      valid values out.  We don't using "mirroring" which might be    */
/*      more suitable for some times of filters.  We also don't mark    */
/*      these pixels as "nodata" though perhaps we should.              */
/* -------------------------------------------------------------------- */
    if( nLeftFill != 0 || nRightFill != 0 )
    {
        for( int i = nTopFill; i < nExtraYSize - nBottomFill; i++ )
        {
            if( nLeftFill != 0 )
                GDALCopyWords( pabyWorkData + nPixelOffset * nLeftFill
                               + i * nLineOffset, eOperDataType, 0, 
                               pabyWorkData + i * nLineOffset, eOperDataType, 
                               nPixelOffset, nLeftFill );

            if( nRightFill != 0 )
                GDALCopyWords( pabyWorkData + i * nLineOffset
                               + nPixelOffset * (nExtraXSize - nRightFill - 1),
                               eOperDataType, 0, 
                               pabyWorkData + i * nLineOffset
                               + nPixelOffset * (nExtraXSize - nRightFill),
                               eOperDataType, nPixelOffset, nRightFill );
        }
    }

    for( int i = 0; i < nTopFill; i++ )
    {
        memcpy( pabyWorkData + i * nLineOffset, 
                pabyWorkData + nTopFill * nLineOffset, 
                nLineOffset );
    }

    for( int i = nExtraYSize - nBottomFill; i < nExtraYSize; i++ )
    {
        memcpy( pabyWorkData + i * nLineOffset, 
                pabyWorkData + (nExtraYSize - nBottomFill - 1) * nLineOffset, 
                nLineOffset );
    }

/* -------------------------------------------------------------------- */
/*      Filter the data.                                                */
/* -------------------------------------------------------------------- */
    eErr = FilterData( nOutXSize, nOutYSize, eOperDataType,
                       pabyWorkData, pabyOutData );

    VSIFree( pabyWorkData );
    if( eErr != CE_None )
    {
        if( pabyOutData != pData )
            VSIFree( pabyOutData );

        return eErr;
    }

/* -------------------------------------------------------------------- */
/*      Copy from work buffer to target buffer.                         */
/* -------------------------------------------------------------------- */
    if( pabyOutData != pData )
    {
        for( int i = 0; i < nOutYSize; i++ )
        {
            GDALCopyWords( pabyOutData + i * (nPixelOffset * nOutXSize),
                           eOperDataType, nPixelOffset,
                           reinterpret_cast<GByte *>( pData ) + i * nLineSpace,
                           eBufType, nPixelSpace, nOutXSize );
        }

        VSIFree( pabyOutData );
    }

    return CE_None;
}

/************************************************************************/
/* ==================================================================== */
/*                       VRTKernelFilteredSource                        */
/* ==================================================================== */
/************************************************************************/

/************************************************************************/
/*                      VRTKernelFilteredSource()                       */
/************************************************************************/

VRTKernelFilteredSource::VRTKernelFilteredSource() :
    nKernelSize(0),
    padfKernelCoefs(NULL),
    bNormalized(FALSE)
{
    GDALDataType aeSupTypes[] = { GDT_Float32 };
    SetFilteringDataTypesSupported( 1, aeSupTypes );
}

/************************************************************************/
/*                      ~VRTKernelFilteredSource()                      */
/************************************************************************/

VRTKernelFilteredSource::~VRTKernelFilteredSource()

{
    CPLFree( padfKernelCoefs );
}

/************************************************************************/
/*                           SetNormalized()                            */
/************************************************************************/

void VRTKernelFilteredSource::SetNormalized( int bNormalizedIn )

{
    bNormalized = bNormalizedIn;
}

/************************************************************************/
/*                             SetKernel()                              */
/************************************************************************/

CPLErr VRTKernelFilteredSource::SetKernel( int nNewKernelSize, 
                                           double *padfNewCoefs )

{
    if( nNewKernelSize < 1 || (nNewKernelSize % 2) != 1 )
    {
        CPLError( CE_Failure, CPLE_AppDefined, 
                  "Illegal filtering kernel size %d, must be odd positive number.", 
                  nNewKernelSize );
        return CE_Failure;
    }

    CPLFree( padfKernelCoefs );
    nKernelSize = nNewKernelSize;

    padfKernelCoefs = reinterpret_cast<double *>(
        CPLMalloc(sizeof(double) * nKernelSize * nKernelSize ) );
    memcpy( padfKernelCoefs, padfNewCoefs,
            sizeof(double) * nKernelSize * nKernelSize );

    SetExtraEdgePixels( (nNewKernelSize - 1) / 2 );

    return CE_None;
}

/************************************************************************/
/*                             FilterData()                             */
/************************************************************************/

CPLErr VRTKernelFilteredSource::
FilterData( int nXSize, int nYSize, GDALDataType eType, 
            GByte *pabySrcData, GByte *pabyDstData )

{
/* -------------------------------------------------------------------- */
/*      Validate data type.                                             */
/* -------------------------------------------------------------------- */
    if( eType != GDT_Float32 )
    {
        CPLError( CE_Failure, CPLE_AppDefined,
                  "Unsupported data type (%s) in "
                  "VRTKernelFilteredSource::FilterData()",
                  GDALGetDataTypeName( eType ) );
        return CE_Failure; 
    }

    CPLAssert( nExtraEdgePixels*2 + 1 == nKernelSize ||
               (nKernelSize == 0 && nExtraEdgePixels == 0) );

/* -------------------------------------------------------------------- */
/*      Float32 case.                                                   */
/* -------------------------------------------------------------------- */
    if( eType == GDT_Float32 )
    {
        int bHasNoData;
        float fNoData
            = static_cast<float>( poRasterBand->GetNoDataValue(&bHasNoData) );

        for( int iY = 0; iY < nYSize; iY++ )
        {
            for( int iX = 0; iX < nXSize; iX++ )
            {
                const int iIndex
                    = (iY+nKernelSize/2 ) * (nXSize+2*nExtraEdgePixels)
                    + iX + nKernelSize/2;
                const float  fCenter
                    = reinterpret_cast<float *>( pabySrcData )[iIndex];

                int iKern = 0;
                double dfSum = 0.0;
                double dfKernSum = 0.0;
                float fResult = 0.0;

                // Check if center srcpixel is NoData
                if(!bHasNoData || fCenter != fNoData)
                {
                    for( int iYY = 0; iYY < nKernelSize; iYY++ )
                    {
                        float *pafData
                            = reinterpret_cast<float *>( pabySrcData )
                            + (iY+iYY) * (nXSize+2*nExtraEdgePixels) + iX;

                        for( int i = 0; i < nKernelSize; i++, pafData++, iKern++ )
                        {
                            if(!bHasNoData || *pafData != fNoData)
                            {
                                dfSum += *pafData * padfKernelCoefs[iKern];
                                dfKernSum += padfKernelCoefs[iKern];
                            }
                        }
                    }
                    if( bNormalized )
                    {
                        if( dfKernSum != 0.0 )
                            fResult = static_cast<float>( dfSum / dfKernSum );
                        else
                            fResult = 0.0;
                    }
                    else
                        fResult = static_cast<float>( dfSum );

                    reinterpret_cast<float *>( pabyDstData )[iX + iY * nXSize]
                        = fResult;
                }
                else
                    reinterpret_cast<float *>( pabyDstData )[iX + iY * nXSize]
                        = fNoData;
            }
        }
    }

    return CE_None;
}

/************************************************************************/
/*                              XMLInit()                               */
/************************************************************************/

CPLErr VRTKernelFilteredSource::XMLInit( CPLXMLNode *psTree, 
                                         const char *pszVRTPath )

{
    CPLErr eErr = VRTFilteredSource::XMLInit( psTree, pszVRTPath );
    if( eErr != CE_None )
        return eErr;

    const int nNewKernelSize = atoi(CPLGetXMLValue(psTree,"Kernel.Size","0"));

    if( nNewKernelSize == 0 )
        return CE_None;

    char **papszCoefItems = 
        CSLTokenizeString( CPLGetXMLValue(psTree,"Kernel.Coefs","") );

    const int nCoefs = CSLCount(papszCoefItems);

    if( nCoefs != nNewKernelSize * nNewKernelSize )
    {
        CSLDestroy( papszCoefItems );
        CPLError( CE_Failure, CPLE_AppDefined, 
                  "Got wrong number of filter kernel coefficients (%s).\n"
                  "Expected %d, got %d.", 
                  CPLGetXMLValue(psTree,"Kernel.Coefs",""),
                  nNewKernelSize * nNewKernelSize, nCoefs );
        return CE_Failure;
    }

    double *padfNewCoefs = reinterpret_cast<double *>(
        CPLMalloc( sizeof(double) * nCoefs ) );

    for( int i = 0; i < nCoefs; i++ )
        padfNewCoefs[i] = CPLAtof(papszCoefItems[i]);

    eErr = SetKernel( nNewKernelSize, padfNewCoefs );

    CPLFree( padfNewCoefs );
    CSLDestroy( papszCoefItems );

    SetNormalized( atoi(CPLGetXMLValue(psTree,"Kernel.normalized","0")) );

    return eErr;
}

/************************************************************************/
/*                           SerializeToXML()                           */
/************************************************************************/

CPLXMLNode *VRTKernelFilteredSource::SerializeToXML( const char *pszVRTPath )

{
    CPLXMLNode *psSrc = VRTFilteredSource::SerializeToXML( pszVRTPath );

    if( psSrc == NULL )
        return NULL;

    CPLFree( psSrc->pszValue );
    psSrc->pszValue = CPLStrdup("KernelFilteredSource" );

    if( nKernelSize == 0 )
        return psSrc;

    CPLXMLNode *psKernel = CPLCreateXMLNode( psSrc, CXT_Element, "Kernel" );

    if( bNormalized )
        CPLCreateXMLNode( 
            CPLCreateXMLNode( psKernel, CXT_Attribute, "normalized" ), 
            CXT_Text, "1" );
    else
        CPLCreateXMLNode( 
            CPLCreateXMLNode( psKernel, CXT_Attribute, "normalized" ), 
            CXT_Text, "0" );

    const int nCoefCount = nKernelSize * nKernelSize;
    char *pszKernelCoefs = reinterpret_cast<char *>(
        CPLMalloc(nCoefCount * 32) );

    strcpy( pszKernelCoefs, "" );
    for( int iCoef = 0; iCoef < nCoefCount; iCoef++ )
        CPLsprintf( pszKernelCoefs + strlen(pszKernelCoefs), 
                 "%.8g ", padfKernelCoefs[iCoef] );

    CPLSetXMLValue( psKernel, "Size", CPLSPrintf( "%d", nKernelSize ) );
    CPLSetXMLValue( psKernel, "Coefs", pszKernelCoefs );

    CPLFree( pszKernelCoefs );

    return psSrc;
}

/************************************************************************/
/*                       VRTParseFilterSources()                        */
/************************************************************************/

VRTSource *VRTParseFilterSources( CPLXMLNode *psChild, const char *pszVRTPath )

{
    if( EQUAL(psChild->pszValue,"KernelFilteredSource") )
    {
        VRTSource *poSrc = new VRTKernelFilteredSource();
        if( poSrc->XMLInit( psChild, pszVRTPath ) == CE_None )
            return poSrc;
        else
            delete poSrc;
    }

    return NULL;
}
