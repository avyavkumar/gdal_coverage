/******************************************************************************
 * $Id$
 *
 * Project:  OpenGIS Simple Features Reference Implementation
 * Purpose:  OGR C API "Spy"
 * Author:   Even Rouault, even.rouault at spatialys.com
 *
 ******************************************************************************
 * Copyright (c) 2014, Even Rouault <even.rouault at spatialys.com>
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

#include "cpl_port.h"

#include <stdio.h>
#include <map>
#include <set>

#include "ograpispy.h"

#include "gdal.h"
#include "ogr_spatialref.h"
#include "ogr_geometry.h"
#include "ogr_feature.h"
#include "ogrsf_frmts.h"
#include "cpl_string.h"

#ifdef OGRAPISPY_ENABLED

int bOGRAPISpyEnabled = FALSE;
static CPLString osSnapshotPath, osSpyFile;
static FILE* fpSpyFile = NULL;

class DatasetDescription
{
    public:
        int iDS;
        std::map<OGRLayerH, int> oMapLayer;

        DatasetDescription() : iDS(-1) {}
        DatasetDescription(int iDS) : iDS(iDS) {}
        ~DatasetDescription();
};

static std::map<OGRDataSourceH, DatasetDescription> oMapDS;
static std::map<OGRLayerH, CPLString> oGlobalMapLayer;
static OGRLayerH hLayerGetNextFeature = NULL;
static OGRLayerH hLayerGetLayerDefn = NULL;
static int nGetNextFeatureCalls = 0;
static std::set<CPLString> aoSetCreatedDS;

DatasetDescription::~DatasetDescription()
{
    std::map<OGRLayerH, int>::iterator oIter = oMapLayer.begin();
    for(; oIter != oMapLayer.end(); ++oIter)
        oGlobalMapLayer.erase(oIter->first);
}

static void OGRAPISpyFileReopen()
{
    if( fpSpyFile == NULL )
    {
        fpSpyFile = fopen(osSpyFile, "ab");
        if( fpSpyFile == NULL )
            fpSpyFile = stderr;
    }
}

static void OGRAPISpyFileClose()
{
    if( fpSpyFile != stdout && fpSpyFile != stderr )
    {
        fclose(fpSpyFile);
        fpSpyFile = NULL;
    }
}

static int OGRAPISpyEnabled()
{
    const char* pszSpyFile = CPLGetConfigOption("OGR_API_SPY_FILE", NULL);
    bOGRAPISpyEnabled = (pszSpyFile != NULL);
    if( !bOGRAPISpyEnabled )
    {
        osSpyFile.resize(0);
        aoSetCreatedDS.clear();
        return FALSE;
    }
    if( osSpyFile.size() )
        return TRUE;

    osSpyFile = pszSpyFile;

    const char* pszSnapshotPath = CPLGetConfigOption("OGR_API_SPY_SNAPSHOT_PATH", ".");
    if( EQUAL(pszSnapshotPath, "NO") )
        osSnapshotPath = "";
    else
        osSnapshotPath = pszSnapshotPath;

    if( EQUAL(pszSpyFile, "stdout") )
        fpSpyFile = stdout;
    else if( EQUAL(pszSpyFile, "stderr") )
        fpSpyFile = stderr;
    else
        fpSpyFile = fopen(pszSpyFile, "wb");
    if( fpSpyFile == NULL )
        fpSpyFile = stderr;

    fprintf(fpSpyFile, "# This file is generated by the OGR_API_SPY mechanism.\n");
    fprintf(fpSpyFile, "from osgeo import ogr\n");
    fprintf(fpSpyFile, "from osgeo import osr\n");
    fprintf(fpSpyFile, "import os\n");
    fprintf(fpSpyFile, "import shutil\n\n");

    return TRUE;
}

static CPLString OGRAPISpyGetOptions(char** papszOptions)
{
    CPLString options;
    if( papszOptions == NULL )
    {
        options = "[]";
    }
    else
    {
        options = "[";
        for(char** papszIter = papszOptions; *papszIter != NULL; papszIter++)
        {
            if( papszIter != papszOptions )
                options += ", ";
            options += "'";
            options += *papszIter;
            options += "'";
        }
        options += "]";
    }

    return options;
}

static CPLString OGRAPISpyGetString(const char* pszStr)
{
    if( pszStr == NULL )
        return "None";
    CPLString osRet = "'";
    while( *pszStr )
    {
        if( *pszStr == '\'' )
            osRet += "\\'";
        else if( *pszStr == '\\' )
            osRet += "\\\\";
        else
            osRet += *pszStr;
        pszStr ++;
    }
    osRet += "'";
    return osRet;
}

static CPLString OGRAPISpyGetDSVar(OGRDataSourceH hDS)
{
    if( hDS && oMapDS.find(hDS) == oMapDS.end() )
    {
        int i = (int)oMapDS.size() + 1;
        oMapDS[hDS] = DatasetDescription(i);
    }
    return CPLSPrintf("ds%d", (hDS) ? oMapDS[hDS].iDS : 0);
}

static CPLString OGRAPISpyGetLayerVar(OGRLayerH hLayer)
{
    return oGlobalMapLayer[hLayer];
}

static CPLString OGRAPISpyGetAndRegisterLayerVar(OGRDataSourceH hDS,
                                                 OGRLayerH hLayer)
{
    DatasetDescription& dd = oMapDS[hDS];
    if( hLayer && dd.oMapLayer.find(hLayer) == dd.oMapLayer.end() )
    {
        int i = (int)dd.oMapLayer.size() + 1;
        dd.oMapLayer[hLayer] = i;
        oGlobalMapLayer[hLayer] = OGRAPISpyGetDSVar(hDS) + "_" + CPLSPrintf("lyr%d", i);
    }
    return OGRAPISpyGetDSVar(hDS) + "_" +
           CPLSPrintf("lyr%d", (hLayer) ? dd.oMapLayer[hLayer] : 0);
}

static CPLString OGRAPISpyGetSRS(OGRSpatialReferenceH hSpatialRef)
{
    if (hSpatialRef == NULL)
        return "None";

    char* pszWKT = NULL;
    ((OGRSpatialReference*)hSpatialRef)->exportToWkt(&pszWKT);
    const char* pszRet = CPLSPrintf("osr.SpatialReference(\"\"\"%s\"\"\")", pszWKT);
    CPLFree(pszWKT);
    return pszRet;
}

static CPLString OGRAPISpyGetGeom(OGRGeometryH hGeom)
{
    if (hGeom == NULL)
        return "None";

    char* pszWKT = NULL;
    ((OGRGeometry*)hGeom)->exportToWkt(&pszWKT);
    const char* pszRet = CPLSPrintf("ogr.CreateGeometryFromWkt('%s')", pszWKT);
    CPLFree(pszWKT);
    return pszRet;
}

#define casePrefixOgrDot(x)  case x: return "ogr." #x;

static CPLString OGRAPISpyGetGeomType(OGRwkbGeometryType eType)
{
    switch(eType)
    {
        casePrefixOgrDot(wkbUnknown)
        casePrefixOgrDot(wkbPoint)
        casePrefixOgrDot(wkbLineString)
        casePrefixOgrDot(wkbPolygon)
        casePrefixOgrDot(wkbMultiPoint)
        casePrefixOgrDot(wkbMultiLineString)
        casePrefixOgrDot(wkbMultiPolygon)
        casePrefixOgrDot(wkbGeometryCollection)
        casePrefixOgrDot(wkbNone)
        casePrefixOgrDot(wkbLinearRing)
        casePrefixOgrDot(wkbPoint25D)
        casePrefixOgrDot(wkbLineString25D)
        casePrefixOgrDot(wkbPolygon25D)
        casePrefixOgrDot(wkbMultiPoint25D)
        casePrefixOgrDot(wkbMultiLineString25D)
        casePrefixOgrDot(wkbMultiPolygon25D)
        casePrefixOgrDot(wkbGeometryCollection25D)
    }
    return "error";
}

static CPLString OGRAPISpyGetFieldType(OGRFieldType eType)
{
    switch(eType)
    {
        casePrefixOgrDot(OFTInteger)
        casePrefixOgrDot(OFTIntegerList)
        casePrefixOgrDot(OFTReal)
        casePrefixOgrDot(OFTRealList)
        casePrefixOgrDot(OFTString)
        casePrefixOgrDot(OFTStringList)
        casePrefixOgrDot(OFTWideString)
        casePrefixOgrDot(OFTWideStringList)
        casePrefixOgrDot(OFTBinary)
        casePrefixOgrDot(OFTDate)
        casePrefixOgrDot(OFTTime)
        casePrefixOgrDot(OFTDateTime)
    }
    return "error";
}

static void OGRAPISpyFlushDiffered()
{
    OGRAPISpyFileReopen();
    if( hLayerGetLayerDefn != NULL )
    {
        fprintf(fpSpyFile, "%s_defn = %s.GetLayerDefn()\n",
            OGRAPISpyGetLayerVar(hLayerGetLayerDefn).c_str(),
            OGRAPISpyGetLayerVar(hLayerGetLayerDefn).c_str());
        hLayerGetLayerDefn = NULL;
    }

    if( nGetNextFeatureCalls == 1)
    {
        fprintf(fpSpyFile, "%s.GetNextFeature()\n",
            OGRAPISpyGetLayerVar(hLayerGetNextFeature).c_str());
        hLayerGetNextFeature = NULL;
        nGetNextFeatureCalls = 0;
    }
    else if( nGetNextFeatureCalls > 0)
    {
        fprintf(fpSpyFile, "for i in range(%d):\n", nGetNextFeatureCalls);
        fprintf(fpSpyFile, "    %s.GetNextFeature()\n",
            OGRAPISpyGetLayerVar(hLayerGetNextFeature).c_str());
        hLayerGetNextFeature = NULL;
        nGetNextFeatureCalls = 0;
    }
}

int OGRAPISpyOpenTakeSnapshot(const char* pszName, int bUpdate)
{
    if( !OGRAPISpyEnabled() || !bUpdate || osSnapshotPath.size() == 0 ||
        aoSetCreatedDS.find(pszName) != aoSetCreatedDS.end() )
        return -1;
    OGRAPISpyFlushDiffered();

    VSIStatBufL sStat;
    if( VSIStatL( pszName, &sStat ) == 0 )
    {
        GDALDatasetH hDS = GDALOpenEx(pszName, GDAL_OF_VECTOR, NULL, NULL, NULL);
        if( hDS )
        {
            char** papszFileList = ((GDALDataset*)hDS)->GetFileList();
            GDALClose(hDS);
            if( papszFileList )
            {
                int i = 1;
                CPLString osBaseDir;
                CPLString osSrcDir;
                CPLString osWorkingDir;
                while(TRUE)
                {
                    osBaseDir = CPLFormFilename(osSnapshotPath,
                                        CPLSPrintf("snapshot_%d", i), NULL );
                    if( VSIStatL( osBaseDir, &sStat ) != 0 )
                        break;
                    i++;
                }
                VSIMkdir( osBaseDir, 0777 );
                osSrcDir = CPLFormFilename( osBaseDir, "source", NULL );
                VSIMkdir( osSrcDir, 0777 );
                osWorkingDir = CPLFormFilename( osBaseDir, "working", NULL );
                VSIMkdir( osWorkingDir, 0777 );
                fprintf(fpSpyFile, "# Take snapshot of %s\n", pszName);
                fprintf(fpSpyFile, "try:\n");
                fprintf(fpSpyFile, "    shutil.rmtree('%s')\n", osWorkingDir.c_str());
                fprintf(fpSpyFile, "except:\n");
                fprintf(fpSpyFile, "    pass\n");
                fprintf(fpSpyFile, "os.mkdir('%s')\n", osWorkingDir.c_str());
                for(char** papszIter = papszFileList; *papszIter; papszIter++)
                {
                    CPLString osSnapshotSrcFile = CPLFormFilename(
                            osSrcDir, CPLGetFilename(*papszIter), NULL);
                    CPLString osSnapshotWorkingFile = CPLFormFilename(
                            osWorkingDir, CPLGetFilename(*papszIter), NULL);
                    CPLCopyFile( osSnapshotSrcFile, *papszIter );
                    CPLCopyFile( osSnapshotWorkingFile, *papszIter );
                    fprintf(fpSpyFile, "shutil.copy('%s', '%s')\n",
                            osSnapshotSrcFile.c_str(),
                            osSnapshotWorkingFile.c_str());
                }
                CSLDestroy(papszFileList);
                return i;
            }
        }
    }
    return -1;
}

void OGRAPISpyOpen(const char* pszName, int bUpdate, int iSnapshot, GDALDatasetH* phDS)
{
    if( !OGRAPISpyEnabled() ) return;
    OGRAPISpyFlushDiffered();

    CPLString osName;
    if( iSnapshot > 0 )
    {
        CPLString osBaseDir = CPLFormFilename(osSnapshotPath,
                                   CPLSPrintf("snapshot_%d", iSnapshot), NULL );
        CPLString osWorkingDir = CPLFormFilename( osBaseDir, "working", NULL );
        osName = CPLFormFilename(osWorkingDir, CPLGetFilename(pszName), NULL);
        pszName = osName.c_str();

        if( *phDS != NULL )
        {
            GDALClose( (GDALDatasetH) *phDS );
            *phDS = GDALOpenEx(pszName, GDAL_OF_VECTOR | GDAL_OF_UPDATE, NULL, NULL, NULL);
        }
    }

    if( *phDS != NULL )
        fprintf(fpSpyFile, "%s = ", OGRAPISpyGetDSVar((OGRDataSourceH) *phDS).c_str());
    fprintf(fpSpyFile, "ogr.Open(%s, update = %d)\n",
            OGRAPISpyGetString(pszName).c_str(), bUpdate);
    OGRAPISpyFileClose();
}

void OGRAPISpyClose(OGRDataSourceH hDS)
{
    OGRAPISpyFlushDiffered();
    fprintf(fpSpyFile, "ds%d = None\n", oMapDS[hDS].iDS);
    oMapDS.erase(hDS);
    OGRAPISpyFileClose();
}

void OGRAPISpyCreateDataSource(OGRSFDriverH hDriver, const char* pszName,
                               char** papszOptions, OGRDataSourceH hDS)
{
    if( !OGRAPISpyEnabled() ) return;
    OGRAPISpyFlushDiffered();
    if( hDS != NULL )
        fprintf(fpSpyFile, "%s = ", OGRAPISpyGetDSVar(hDS).c_str());
    fprintf(fpSpyFile, "ogr.GetDriverByName('%s').CreateDataSource(%s, options = %s)\n",
            GDALGetDriverShortName((GDALDriverH)hDriver),
            OGRAPISpyGetString(pszName).c_str(),
            OGRAPISpyGetOptions(papszOptions).c_str());
    if( hDS != NULL )
    {
        aoSetCreatedDS.insert(pszName);
    }
    OGRAPISpyFileClose();
}

void OGRAPISpyDeleteDataSource(OGRSFDriverH hDriver, const char* pszName)
{
    if( !OGRAPISpyEnabled() ) return;
    OGRAPISpyFlushDiffered();
    fprintf(fpSpyFile, "ogr.GetDriverByName('%s').DeleteDataSource(%s)\n",
            GDALGetDriverShortName((GDALDriverH)hDriver),
            OGRAPISpyGetString(pszName).c_str());
    aoSetCreatedDS.erase(pszName);
    OGRAPISpyFileClose();
}

void OGRAPISpy_DS_GetLayer( OGRDataSourceH hDS, int iLayer, OGRLayerH hLayer )
{
    OGRAPISpyFlushDiffered();
    if( hLayer != NULL )
        fprintf(fpSpyFile, "%s = ",
            OGRAPISpyGetAndRegisterLayerVar(hDS, hLayer).c_str());
    fprintf(fpSpyFile, "%s.GetLayer(%d)\n",
            OGRAPISpyGetDSVar(hDS).c_str(),
            iLayer);
    OGRAPISpyFileClose();
}

void OGRAPISpy_DS_GetLayerCount( OGRDataSourceH hDS )
{
    OGRAPISpyFlushDiffered();
    fprintf(fpSpyFile, "%s.GetLayerCount()\n", OGRAPISpyGetDSVar(hDS).c_str());
    OGRAPISpyFileClose();
}

void OGRAPISpy_DS_GetLayerByName( OGRDataSourceH hDS, const char* pszLayerName,
                                  OGRLayerH hLayer )
{
    OGRAPISpyFlushDiffered();
    if( hLayer != NULL )
        fprintf(fpSpyFile, "%s = ",
            OGRAPISpyGetAndRegisterLayerVar(hDS, hLayer).c_str());
    fprintf(fpSpyFile, "%s.GetLayerByName(%s)\n",
            OGRAPISpyGetDSVar(hDS).c_str(),
            OGRAPISpyGetString(pszLayerName).c_str());
    OGRAPISpyFileClose();
}

void OGRAPISpy_DS_ExecuteSQL( OGRDataSourceH hDS, 
                              const char *pszStatement,
                              OGRGeometryH hSpatialFilter,
                              const char *pszDialect,
                              OGRLayerH hLayer)
{
    OGRAPISpyFlushDiffered();
    if( hLayer != NULL )
        fprintf(fpSpyFile, "%s = ",
            OGRAPISpyGetAndRegisterLayerVar(hDS, hLayer).c_str());
    fprintf(fpSpyFile, "%s.ExecuteSQL(%s, %s, %s)\n",
            OGRAPISpyGetDSVar(hDS).c_str(),
            OGRAPISpyGetString(pszStatement).c_str(),
            OGRAPISpyGetGeom(hSpatialFilter).c_str(),
            OGRAPISpyGetString(pszDialect).c_str());
    OGRAPISpyFileClose();
}

void OGRAPISpy_DS_ReleaseResultSet( OGRDataSourceH hDS, OGRLayerH hLayer)
{
    OGRAPISpyFlushDiffered();
    fprintf(fpSpyFile, "%s.ReleaseResultSet(%s)\n",
            OGRAPISpyGetDSVar(hDS).c_str(),
            (hLayer) ? OGRAPISpyGetLayerVar(hLayer).c_str() : "None");

    DatasetDescription& dd = oMapDS[hDS];
    dd.oMapLayer.erase(hLayer);
    oGlobalMapLayer.erase(hLayer);

    OGRAPISpyFileClose();
}

void OGRAPISpy_DS_CreateLayer( OGRDataSourceH hDS, 
                               const char * pszName,
                               OGRSpatialReferenceH hSpatialRef,
                               OGRwkbGeometryType eType,
                               char ** papszOptions,
                               OGRLayerH hLayer)
{
    OGRAPISpyFlushDiffered();
    if( hLayer != NULL )
        fprintf(fpSpyFile, "%s = ",
            OGRAPISpyGetAndRegisterLayerVar(hDS, hLayer).c_str());
    fprintf(fpSpyFile, "%s.CreateLayer(%s, srs = %s, geom_type = %s, options = %s)\n",
            OGRAPISpyGetDSVar(hDS).c_str(),
            OGRAPISpyGetString(pszName).c_str(),
            OGRAPISpyGetSRS(hSpatialRef).c_str(),
            OGRAPISpyGetGeomType(eType).c_str(),
            OGRAPISpyGetOptions(papszOptions).c_str());
    OGRAPISpyFileClose();
}

void OGRAPISpy_DS_DeleteLayer( OGRDataSourceH hDS, int iLayer, OGRErr eErr )
{
    OGRAPISpyFlushDiffered();
    fprintf(fpSpyFile, "%s.DeleteLayer(%d)\n",
            OGRAPISpyGetDSVar(hDS).c_str(), iLayer);
    // Should perhaps remove from the maps
    OGRAPISpyFileClose();
}


void OGRAPISpy_L_GetFeatureCount( OGRLayerH hLayer, int bForce )
{
    OGRAPISpyFlushDiffered();
    fprintf(fpSpyFile, "%s.GetFeatureCount(force = %d)\n",
            OGRAPISpyGetLayerVar(hLayer).c_str(), bForce);
    OGRAPISpyFileClose();
}

void OGRAPISpy_L_GetExtent( OGRLayerH hLayer, int bForce )
{
    OGRAPISpyFlushDiffered();
    fprintf(fpSpyFile, "%s.GetExtent(force = %d)\n",
            OGRAPISpyGetLayerVar(hLayer).c_str(), bForce);
    OGRAPISpyFileClose();
}

void OGRAPISpy_L_GetExtentEx( OGRLayerH hLayer, int iGeomField, int bForce )
{
    OGRAPISpyFlushDiffered();
    fprintf(fpSpyFile, "%s.GetExtent(geom_field = %d, force = %d)\n",
            OGRAPISpyGetLayerVar(hLayer).c_str(), iGeomField, bForce);
    OGRAPISpyFileClose();
}

void OGRAPISpy_L_SetAttributeFilter( OGRLayerH hLayer, const char* pszFilter )
{
    OGRAPISpyFlushDiffered();
    fprintf(fpSpyFile, "%s.SetAttributeFilter(%s)\n",
            OGRAPISpyGetLayerVar(hLayer).c_str(),
            OGRAPISpyGetString(pszFilter).c_str());
    OGRAPISpyFileClose();
}

void OGRAPISpy_L_GetFeature( OGRLayerH hLayer, long nFeatureId )
{
    OGRAPISpyFlushDiffered();
    fprintf(fpSpyFile, "%s.GetFeature(%ld)\n",
            OGRAPISpyGetLayerVar(hLayer).c_str(), nFeatureId);
    OGRAPISpyFileClose();
}

void OGRAPISpy_L_SetNextByIndex( OGRLayerH hLayer, long nIndex )
{
    OGRAPISpyFlushDiffered();
    fprintf(fpSpyFile, "%s.SetNextByIndex(%ld)\n",
            OGRAPISpyGetLayerVar(hLayer).c_str(), nIndex);
    OGRAPISpyFileClose();
}

void OGRAPISpy_L_GetNextFeature( OGRLayerH hLayer )
{
    if( hLayerGetNextFeature != hLayer )
    {
        OGRAPISpyFlushDiffered();
        OGRAPISpyFileClose();
    }
    hLayerGetNextFeature = hLayer;
    nGetNextFeatureCalls++;
}

static void OGRAPISpyDumpFeature( OGRLayerH hLayer, OGRFeatureH hFeat )
{
    OGRLayer* poLayer = (OGRLayer*) hLayer;
    OGRFeature* poFeature = (OGRFeature*) hFeat;
    /* Do not check pointer equality, since the Perl bindings can */
    /* build features with a OGRFeatureDefn that is a copy of the layer defn */
    CPLAssert(poFeature->GetDefnRef()->IsSame(poLayer->GetLayerDefn()));
    fprintf(fpSpyFile, "f = ogr.Feature(%s_defn)\n",
            OGRAPISpyGetLayerVar(hLayer).c_str());
    if( poFeature->GetFID() != -1 )
        fprintf(fpSpyFile, "f.SetFID(%ld)\n", poFeature->GetFID());
    int i;
    for(i = 0; i < poFeature->GetFieldCount(); i++)
    {
        if( poFeature->IsFieldSet(i) )
        {
            switch( poFeature->GetFieldDefnRef(i)->GetType())
            {
                case OFTInteger: fprintf(fpSpyFile, "f.SetField(%d, %d)\n", i,
                    poFeature->GetFieldAsInteger(i)); break;
                case OFTReal: fprintf(fpSpyFile, "f.SetField(%d, %.16g)\n", i,
                    poFeature->GetFieldAsDouble(i)); break;
                case OFTString: fprintf(fpSpyFile, "f.SetField(%d, %s)\n", i,
                    OGRAPISpyGetString(poFeature->GetFieldAsString(i)).c_str()); break;
                default: fprintf(fpSpyFile, "f.SetField(%d, %s) #FIXME\n", i,
                    OGRAPISpyGetString(poFeature->GetFieldAsString(i)).c_str()); break;
            }
        }
    }
    for(i = 0; i < poFeature->GetGeomFieldCount(); i++)
    {
        OGRGeometry* poGeom = poFeature->GetGeomFieldRef(i);
        if( poGeom != NULL )
        {
            fprintf(fpSpyFile, "f.SetGeomField(%d, %s)\n", i, OGRAPISpyGetGeom(
                (OGRGeometryH)poGeom ).c_str() ); 
        }
    }
    const char* pszStyleString = poFeature->GetStyleString();
    if( pszStyleString != NULL )
        fprintf(fpSpyFile, "f.SetStyleString(%s)\n",
                OGRAPISpyGetString(pszStyleString).c_str() ); 
}

void OGRAPISpy_L_SetFeature( OGRLayerH hLayer, OGRFeatureH hFeat )
{
    OGRAPISpyFlushDiffered();
    OGRAPISpyDumpFeature(hLayer, hFeat);
    fprintf(fpSpyFile, "%s.SetFeature(f)\n",
            OGRAPISpyGetLayerVar(hLayer).c_str());
    fprintf(fpSpyFile, "f = None\n"); /* in case layer defn is changed afterwards */
    OGRAPISpyFileClose();
}

void OGRAPISpy_L_CreateFeature( OGRLayerH hLayer, OGRFeatureH hFeat )
{
    OGRAPISpyFlushDiffered();
    OGRAPISpyDumpFeature(hLayer, hFeat);
    fprintf(fpSpyFile, "%s.CreateFeature(f)\n",
            OGRAPISpyGetLayerVar(hLayer).c_str());
    fprintf(fpSpyFile, "f = None\n"); /* in case layer defn is changed afterwards */
    OGRAPISpyFileClose();
}

static void OGRAPISpyDumpFieldDefn( OGRFieldDefn* poFieldDefn )
{
    fprintf(fpSpyFile, "fd = ogr.FieldDefn(%s, %s)\n",
            OGRAPISpyGetString(poFieldDefn->GetNameRef()).c_str(),
            OGRAPISpyGetFieldType(poFieldDefn->GetType()).c_str());
    if( poFieldDefn->GetWidth() > 0 )
        fprintf(fpSpyFile, "fd.SetWidth(%d)\n", poFieldDefn->GetWidth() );
    if( poFieldDefn->GetPrecision() > 0 )
        fprintf(fpSpyFile, "fd.SetPrecision(%d)\n", poFieldDefn->GetPrecision() );
}

void OGRAPISpy_L_CreateField( OGRLayerH hLayer, OGRFieldDefnH hField, 
                              int bApproxOK )
{
    OGRAPISpyFlushDiffered();
    OGRFieldDefn* poFieldDefn = (OGRFieldDefn*) hField;
    OGRAPISpyDumpFieldDefn(poFieldDefn);
    fprintf(fpSpyFile, "%s.CreateField(fd, approx_ok = %d)\n",
            OGRAPISpyGetLayerVar(hLayer).c_str(), bApproxOK);
    OGRAPISpyFileClose();
}

void OGRAPISpy_L_DeleteField( OGRLayerH hLayer, int iField )
{
    OGRAPISpyFlushDiffered();
    fprintf(fpSpyFile, "%s.DeleteField(%d)\n",
            OGRAPISpyGetLayerVar(hLayer).c_str(), iField);
    OGRAPISpyFileClose();
}

void OGRAPISpy_L_ReorderFields( OGRLayerH hLayer, int* panMap )
{
    OGRAPISpyFlushDiffered();
    OGRLayer* poLayer = (OGRLayer*) hLayer;
    fprintf(fpSpyFile, "%s.ReorderFields([",
            OGRAPISpyGetLayerVar(hLayer).c_str());
    for( int i = 0; i < poLayer->GetLayerDefn()->GetFieldCount(); i++ )
    {
        if( i > 0 ) fprintf(fpSpyFile, ", ");
        fprintf(fpSpyFile, "%d", panMap[i]);
    }
    fprintf(fpSpyFile, "])\n");
    OGRAPISpyFileClose();
}

void OGRAPISpy_L_ReorderField( OGRLayerH hLayer, int iOldFieldPos,
                               int iNewFieldPos )
{
    OGRAPISpyFlushDiffered();
    fprintf(fpSpyFile, "%s.ReorderField(%d, %d)\n",
            OGRAPISpyGetLayerVar(hLayer).c_str(), iOldFieldPos, iNewFieldPos);
    OGRAPISpyFileClose();
}

void OGRAPISpy_L_AlterFieldDefn( OGRLayerH hLayer, int iField,
                                 OGRFieldDefnH hNewFieldDefn, int nFlags )
{
    OGRAPISpyFlushDiffered();
    OGRFieldDefn* poFieldDefn = (OGRFieldDefn*) hNewFieldDefn;
    OGRAPISpyDumpFieldDefn(poFieldDefn);
    fprintf(fpSpyFile, "%s.AlterFieldDefn(%d, fd, %d)\n",
            OGRAPISpyGetLayerVar(hLayer).c_str(), iField, nFlags);
    OGRAPISpyFileClose();
}

void OGRAPISpy_L_CreateGeomField( OGRLayerH hLayer, OGRGeomFieldDefnH hField, 
                                  int bApproxOK )
{
    OGRAPISpyFlushDiffered();
    OGRGeomFieldDefn* poGeomFieldDefn = (OGRGeomFieldDefn*) hField;
    fprintf(fpSpyFile, "geom_fd = ogr.GeomFieldDefn(%s, %s)\n",
            OGRAPISpyGetString(poGeomFieldDefn->GetNameRef()).c_str(),
            OGRAPISpyGetGeomType(poGeomFieldDefn->GetType()).c_str());
    if( poGeomFieldDefn->GetSpatialRef() != NULL )
        fprintf(fpSpyFile, "geom_fd.SetSpatialRef(%s)\n", OGRAPISpyGetSRS(
            (OGRSpatialReferenceH)poGeomFieldDefn->GetSpatialRef()).c_str() );
    fprintf(fpSpyFile, "%s.CreateGeomField(geom_fd, approx_ok = %d)\n",
            OGRAPISpyGetLayerVar(hLayer).c_str(), bApproxOK);
    OGRAPISpyFileClose();
}

static void OGRAPISpy_L_Op( OGRLayerH hLayer, const char* pszMethod )
{
    OGRAPISpyFlushDiffered();
    fprintf(fpSpyFile, "%s.%s()\n", OGRAPISpyGetLayerVar(hLayer).c_str(), pszMethod);
    OGRAPISpyFileClose();
}

void OGRAPISpy_L_StartTransaction( OGRLayerH hLayer ) { OGRAPISpy_L_Op(hLayer, "StartTransaction"); }
void OGRAPISpy_L_CommitTransaction( OGRLayerH hLayer ) { OGRAPISpy_L_Op(hLayer, "CommitTransaction"); }
void OGRAPISpy_L_RollbackTransaction( OGRLayerH hLayer ) { OGRAPISpy_L_Op(hLayer, "RollbackTransaction"); }

void OGRAPISpy_L_GetLayerDefn( OGRLayerH hLayer )
{
    if( hLayer != hLayerGetLayerDefn )
    {
        OGRAPISpyFlushDiffered();
        hLayerGetLayerDefn = hLayer;
        OGRAPISpyFileClose();
    }
}

void OGRAPISpy_L_GetSpatialRef( OGRLayerH hLayer ) { OGRAPISpy_L_Op(hLayer, "GetSpatialRef"); }
void OGRAPISpy_L_GetSpatialFilter( OGRLayerH hLayer ) { OGRAPISpy_L_Op(hLayer, "GetSpatialFilter"); }
void OGRAPISpy_L_ResetReading( OGRLayerH hLayer ) { OGRAPISpy_L_Op(hLayer, "ResetReading"); }
void OGRAPISpy_L_SyncToDisk( OGRLayerH hLayer ) { OGRAPISpy_L_Op(hLayer, "SyncToDisk"); }
void OGRAPISpy_L_GetFIDColumn( OGRLayerH hLayer ) { OGRAPISpy_L_Op(hLayer, "GetFIDColumn"); }
void OGRAPISpy_L_GetGeometryColumn( OGRLayerH hLayer ) { OGRAPISpy_L_Op(hLayer, "GetGeometryColumn"); }
void OGRAPISpy_L_GetName( OGRLayerH hLayer ) { OGRAPISpy_L_Op(hLayer, "GetName"); }
void OGRAPISpy_L_GetGeomType( OGRLayerH hLayer ) { OGRAPISpy_L_Op(hLayer, "GetGeomType"); }

void OGRAPISpy_L_FindFieldIndex( OGRLayerH hLayer, const char *pszFieldName,
                                 int bExactMatch )
{
    OGRAPISpyFlushDiffered();
    fprintf(fpSpyFile, "%s.FindFieldIndex(%s, %d)\n",
            OGRAPISpyGetLayerVar(hLayer).c_str(),
            OGRAPISpyGetString(pszFieldName).c_str(), bExactMatch);
    OGRAPISpyFileClose();
}

void OGRAPISpy_L_TestCapability( OGRLayerH hLayer, const char* pszCap )
{
    OGRAPISpyFlushDiffered();
    fprintf(fpSpyFile, "%s.TestCapability(%s)\n",
            OGRAPISpyGetLayerVar(hLayer).c_str(),
            OGRAPISpyGetString(pszCap).c_str());
    OGRAPISpyFileClose();
}

void OGRAPISpy_L_SetSpatialFilter( OGRLayerH hLayer, OGRGeometryH hGeom )
{
    OGRAPISpyFlushDiffered();
    fprintf(fpSpyFile, "%s.SetSpatialFilter(%s)\n",
            OGRAPISpyGetLayerVar(hLayer).c_str(),
            OGRAPISpyGetGeom(hGeom).c_str());
    OGRAPISpyFileClose();
}

void OGRAPISpy_L_SetSpatialFilterEx( OGRLayerH hLayer, int iGeomField,
                                     OGRGeometryH hGeom )
{
    OGRAPISpyFlushDiffered();
    fprintf(fpSpyFile, "%s.SetSpatialFilter(%d, %s)\n",
            OGRAPISpyGetLayerVar(hLayer).c_str(),
            iGeomField,
            OGRAPISpyGetGeom(hGeom).c_str());
    OGRAPISpyFileClose();
}

void OGRAPISpy_L_SetSpatialFilterRect( OGRLayerH hLayer,
                                       double dfMinX, double dfMinY, 
                                       double dfMaxX, double dfMaxY)
{
    OGRAPISpyFlushDiffered();
    fprintf(fpSpyFile, "%s.SetSpatialFilterRect(%.16g, %.16g, %.16g, %.16g)\n",
            OGRAPISpyGetLayerVar(hLayer).c_str(),
            dfMinX, dfMinY, dfMaxX, dfMaxY);
    OGRAPISpyFileClose();
}

void OGRAPISpy_L_SetSpatialFilterRectEx( OGRLayerH hLayer, int iGeomField,
                                         double dfMinX, double dfMinY, 
                                         double dfMaxX, double dfMaxY)
{

    OGRAPISpyFlushDiffered();
    fprintf(fpSpyFile, "%s.SetSpatialFilterRect(%d, "
            "%.16g, %.16g, %.16g, %.16g)\n",
            OGRAPISpyGetLayerVar(hLayer).c_str(),
            iGeomField,
            dfMinX, dfMinY, dfMaxX, dfMaxY);
    OGRAPISpyFileClose();
}

void OGRAPISpy_L_DeleteFeature( OGRLayerH hLayer, long nFID )
{
    OGRAPISpyFlushDiffered();
    fprintf(fpSpyFile, "%s.DeleteFeature(%ld)\n",
            OGRAPISpyGetLayerVar(hLayer).c_str(), nFID);
    OGRAPISpyFileClose();
}

void OGRAPISpy_L_SetIgnoredFields( OGRLayerH hLayer,
                                   const char** papszIgnoredFields )
{
    OGRAPISpyFlushDiffered();
    fprintf(fpSpyFile, "%s.SetIgnoredFields(%s)\n",
            OGRAPISpyGetLayerVar(hLayer).c_str(),
            OGRAPISpyGetOptions((char**)papszIgnoredFields).c_str());
    OGRAPISpyFileClose();
}

#endif /* OGRAPISPY_ENABLED */
