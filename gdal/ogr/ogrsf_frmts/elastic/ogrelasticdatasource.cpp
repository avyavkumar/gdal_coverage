/******************************************************************************
 * $Id$
 *
 * Project:  ElasticSearch Translator
 * Purpose:
 * Author:
 *
 ******************************************************************************
 * Copyright (c) 2011, Adam Estrada
 * Copyright (c) 2012, Even Rouault <even dot rouault at mines-paris dot org>
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

// What was this supposed to do?
// #pragma warning( disable : 4251 )

#include "ogr_elastic.h"
#include "cpl_conv.h"
#include "cpl_string.h"
#include "cpl_csv.h"
#include "cpl_http.h"

CPL_CVSID("$Id$");

/************************************************************************/
/*                        OGRElasticDataSource()                        */
/************************************************************************/

OGRElasticDataSource::OGRElasticDataSource() {
    papoLayers = NULL;
    nLayers = 0;
    pszName = NULL;
    pszMapping = NULL;
    pszWriteMap = NULL;
}

/************************************************************************/
/*                       ~OGRElasticDataSource()                        */
/************************************************************************/

OGRElasticDataSource::~OGRElasticDataSource() {
    for (int i = 0; i < nLayers; i++)
        delete papoLayers[i];
    CPLFree(papoLayers);
    CPLFree(pszName);
    CPLFree(pszMapping);
    CPLFree(pszWriteMap);
}

/************************************************************************/
/*                           TestCapability()                           */
/************************************************************************/

int OGRElasticDataSource::TestCapability(const char * pszCap) {
    if (EQUAL(pszCap, ODsCCreateLayer) ||
        EQUAL(pszCap, ODsCCreateGeomFieldAfterCreateLayer) )
        return TRUE;
    else
        return FALSE;
}

/************************************************************************/
/*                              GetLayer()                              */
/************************************************************************/

OGRLayer *OGRElasticDataSource::GetLayer(int iLayer) {
    if (iLayer < 0 || iLayer >= nLayers)
        return NULL;
    else
        return papoLayers[iLayer];
}

/************************************************************************/
/*                           ICreateLayer()                             */
/************************************************************************/

OGRLayer * OGRElasticDataSource::ICreateLayer(const char * pszLayerName,
                                              OGRSpatialReference *poSRS,
                                              OGRwkbGeometryType eGType,
                                              char ** papszOptions)
{
    CPLString osLaunderedName(pszLayerName);
    for(size_t i=0;i<osLaunderedName.size();i++)
    {
        if( osLaunderedName[i] >= 'A' && osLaunderedName[i] <= 'Z' )
            osLaunderedName[i] += 'a' - 'A';
        else if( osLaunderedName[i] == '/' || osLaunderedName[i] == '?' )
            osLaunderedName[i] = '_';
    }
    if( strcmp(osLaunderedName.c_str(), pszLayerName) != 0 )
        CPLDebug("ES", "Laundered layer name to %s", osLaunderedName.c_str());

    if( bOverwrite || CSLFetchBoolean(papszOptions, "OVERWRITE", FALSE) )
    {
        // Check if the index exists
        CPLPushErrorHandler(CPLQuietErrorHandler);
        CPLHTTPResult* psResult = CPLHTTPFetch(CPLSPrintf("%s/%s", GetURL(), osLaunderedName.c_str()), NULL);
        CPLPopErrorHandler();
        if (psResult) {
            int bOK = (psResult->pszErrBuf == NULL);
            CPLHTTPDestroyResult(psResult);
            if( bOK )
            {
                // Then delete it
                DeleteIndex(CPLSPrintf("%s/%s", GetURL(), osLaunderedName.c_str()));
            }
        }
    }
    
    // Create the index
    if( !UploadFile(CPLSPrintf("%s/%s", GetURL(), osLaunderedName.c_str()), "") )
        return NULL;

    // If we have a user specified mapping, then go ahead and update it now
    const char* pszLayerMapping = pszMapping;
    if (pszLayerMapping != NULL) {
        if( !UploadFile(CPLSPrintf("%s/%s/FeatureCollection/_mapping", GetURL(), osLaunderedName.c_str()),
                   pszLayerMapping) )
        {
            return NULL;
        }
    }
    
    OGRElasticLayer* poLayer = new OGRElasticLayer(osLaunderedName.c_str(), this, papszOptions);
    nLayers++;
    papoLayers = (OGRElasticLayer **) CPLRealloc(papoLayers, nLayers * sizeof (OGRElasticLayer*));
    papoLayers[nLayers - 1] = poLayer;
    
    if( eGType != wkbNone )
    {
        const char* pszGeometryName = CSLFetchNameValueDef(papszOptions, "GEOMETRY_NAME", "geometry");
        OGRGeomFieldDefn oFieldDefn(pszGeometryName, eGType);
        oFieldDefn.SetSpatialRef(poSRS);
        poLayer->CreateGeomField(&oFieldDefn, FALSE);
    }

    return poLayer;
}

/************************************************************************/
/*                               RunRequest()                           */
/************************************************************************/

json_object* OGRElasticDataSource::RunRequest(const char* pszURL, const char* pszPostContent)
{
    char** papszOptions = NULL;
    
    if( pszPostContent )
    {
        papszOptions = CSLSetNameValue(papszOptions, "POSTFIELDS",
                                       pszPostContent);
    }

    CPLHTTPResult * psResult = CPLHTTPFetch( pszURL, papszOptions );
    CSLDestroy(papszOptions);

    if( psResult->pszErrBuf != NULL )
    {
        CPLError(CE_Failure, CPLE_AppDefined, "%s",
                psResult->pabyData ? (const char*) psResult->pabyData :
                psResult->pszErrBuf);

        CPLHTTPDestroyResult(psResult);
        return NULL;
    }

    if( psResult->pabyData == NULL )
    {
        CPLError(CE_Failure, CPLE_AppDefined, "Empty content returned by server");
        CPLHTTPDestroyResult(psResult);
        return NULL;
    }

    json_tokener* jstok = NULL;
    json_object* poObj = NULL;

    jstok = json_tokener_new();
    poObj = json_tokener_parse_ex(jstok, (const char*) psResult->pabyData, -1);
    if( jstok->err != json_tokener_success)
    {
        CPLError( CE_Failure, CPLE_AppDefined,
                    "JSON parsing error: %s (at offset %d)",
                    json_tokener_error_desc(jstok->err), jstok->char_offset);
        json_tokener_free(jstok);
        CPLHTTPDestroyResult(psResult);
        return NULL;
    }
    json_tokener_free(jstok);

    CPLHTTPDestroyResult(psResult);

    if( json_object_get_type(poObj) != json_type_object )
    {
        CPLError( CE_Failure, CPLE_AppDefined, "Return is not a JSON dictionary");
        json_object_put(poObj);
        poObj = NULL;
    }
    
    return poObj;
}

/************************************************************************/
/*                                Open()                                */
/************************************************************************/

int OGRElasticDataSource::Open(GDALOpenInfo* poOpenInfo) {

    pszName = CPLStrdup(poOpenInfo->pszFilename);
    osURL = (EQUALN(pszName, "ES:", 3)) ? pszName + 3 : pszName;
    
    CPLHTTPResult* psResult = CPLHTTPFetch((osURL + "/_cat/indices?h=i").c_str(), NULL);
    if( psResult == NULL || psResult->pabyData == NULL || psResult->pszErrBuf != NULL )
    {
        CPLHTTPDestroyResult(psResult);
        return FALSE;
    }

    char* pszCur = (char*)psResult->pabyData;
    char* pszNextEOL = strchr(pszCur, '\n');
    while( pszNextEOL && pszNextEOL > pszCur )
    {
        *pszNextEOL = '\0';
        
        char* pszBeforeEOL = pszNextEOL - 1;
        while( *pszBeforeEOL == ' ' )
        {
            *pszBeforeEOL = '\0';
            pszBeforeEOL  --;
        }

        const char* pszIndexName = pszCur;

        json_object* poRes = RunRequest((osURL + CPLString("/") + pszIndexName + CPLString("?pretty")).c_str());
        if( poRes )
        {
            json_object* poLayerObj = json_object_object_get(poRes, pszIndexName);
            json_object* poMappings = NULL;
            if( poLayerObj && json_object_get_type(poLayerObj) == json_type_object )
                poMappings = json_object_object_get(poLayerObj, "mappings");
            if( poMappings && json_object_get_type(poMappings) == json_type_object )
            {
                json_object_iter it;
                it.key = NULL;
                it.val = NULL;
                it.entry = NULL;
                std::vector<CPLString> aosMappings;
                json_object_object_foreachC( poMappings, it )
                {
                    aosMappings.push_back(it.key);
                }
                if( aosMappings.size() == 1 && aosMappings[0] == "FeatureCollection" )
                {
                    OGRElasticLayer* poLayer = new OGRElasticLayer(pszCur, this, NULL);
                    poLayer->BuildFeatureCollectionSchema(json_object_object_get(poMappings, "FeatureCollection"));

                    nLayers++;
                    papoLayers = (OGRElasticLayer **) CPLRealloc(papoLayers, nLayers * sizeof (OGRElasticLayer*));
                    papoLayers[nLayers - 1] = poLayer;
                }
            }
            
            json_object_put(poRes);
        }

        pszCur = pszNextEOL + 1;
        pszNextEOL = strchr(pszCur, '\n');
    }

    CPLHTTPDestroyResult(psResult);
    return TRUE;
}


/************************************************************************/
/*                             DeleteIndex()                            */
/************************************************************************/

void OGRElasticDataSource::DeleteIndex(const CPLString &url) {
    char** papszOptions = NULL;
    papszOptions = CSLAddNameValue(papszOptions, "CUSTOMREQUEST", "DELETE");
    CPLHTTPResult* psResult = CPLHTTPFetch(url, papszOptions);
    CSLDestroy(papszOptions);
    if (psResult) {
        CPLHTTPDestroyResult(psResult);
    }
}

/************************************************************************/
/*                            UploadFile()                              */
/************************************************************************/

int OGRElasticDataSource::UploadFile(const CPLString &url, const CPLString &data) {
    int bRet = TRUE;
    char** papszOptions = NULL;
    papszOptions = CSLAddNameValue(papszOptions, "POSTFIELDS", data.c_str());
    papszOptions = CSLAddNameValue(papszOptions, "HEADERS",
            "Content-Type: application/x-javascript; charset=UTF-8");

    CPLHTTPResult* psResult = CPLHTTPFetch(url, papszOptions);
    CSLDestroy(papszOptions);
    if (psResult) {
        if( psResult->pszErrBuf != NULL ||
            (psResult->pabyData && strncmp((const char*) psResult->pabyData, "{\"error\":", strlen("{\"error\":")) == 0) )
        {
            bRet = FALSE;
            CPLError(CE_Failure, CPLE_AppDefined, "%s",
                        psResult->pabyData ? (const char*) psResult->pabyData :
                        psResult->pszErrBuf);
        }
        CPLHTTPDestroyResult(psResult);
    }
    return bRet;
}

/************************************************************************/
/*                               Create()                               */
/************************************************************************/

int OGRElasticDataSource::Create(const char *pszFilename,
                                 CPL_UNUSED char **papszOptions) {
    this->pszName = CPLStrdup(pszFilename);
    osURL = (EQUALN(pszFilename, "ES:", 3)) ? pszFilename + 3 : pszFilename;

    const char* pszMetaFile = CPLGetConfigOption("ES_META", NULL);
    const char* pszWriteMap = CPLGetConfigOption("ES_WRITEMAP", NULL);;
    this->bOverwrite = CSLTestBoolean(CPLGetConfigOption("ES_OVERWRITE", "0"));
    this->nBulkUpload = (int) CPLAtof(CPLGetConfigOption("ES_BULK", "0"));

    if (pszWriteMap != NULL) {
        this->pszWriteMap = CPLStrdup(pszWriteMap);
    }

    // Read in the meta file from disk
    if (pszMetaFile != NULL)
    {
        int fsize;
        char *fdata;
        FILE *fp;

        fp = fopen(pszMetaFile, "rb");
        if (fp != NULL) {
            fseek(fp, 0, SEEK_END);
            fsize = (int) ftell(fp);

            fdata = (char *) malloc(fsize + 1);

            fseek(fp, 0, SEEK_SET);
            if (0 == fread(fdata, fsize, 1, fp)) {
                CPLError(CE_Failure, CPLE_FileIO,
                         "OGRElasticDataSource::Create read failed.");
            }
            fdata[fsize] = 0;
            this->pszMapping = fdata;
            fclose(fp);
        }
    }

    // Do a status check to ensure that the server is valid
    CPLHTTPResult* psResult = CPLHTTPFetch(CPLSPrintf("%s/_status", osURL.c_str()), NULL);
    int bOK = (psResult != NULL && psResult->pszErrBuf == NULL);
    if (!bOK)
    {
        CPLError(CE_Failure, CPLE_NoWriteAccess,
                "Could not connect to server");
    }

    CPLHTTPDestroyResult(psResult);

    return bOK;
}
