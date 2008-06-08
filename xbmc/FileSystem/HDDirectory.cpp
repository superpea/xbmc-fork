/*
 *      Copyright (C) 2005-2008 Team XBMC
 *      http://www.xbmc.org
 *
 *  This Program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2, or (at your option)
 *  any later version.
 *
 *  This Program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with XBMC; see the file COPYING.  If not, write to
 *  the Free Software Foundation, 675 Mass Ave, Cambridge, MA 02139, USA.
 *  http://www.gnu.org/copyleft/gpl.html
 *
 */

#ifdef __APPLE__
#include <CoreServices/CoreServices.h>
#endif
#include "stdafx.h"
#include "HDDirectory.h"
#include "Util.h"
#include "xbox/IoSupport.h"
#include "DirectoryCache.h"
#include "iso9660.h"
#include "URL.h"
#include "GUISettings.h"
#include "FileItem.h"

#ifndef INVALID_FILE_ATTRIBUTES
#define INVALID_FILE_ATTRIBUTES ((DWORD) -1)
#endif

using namespace AUTOPTR;
using namespace DIRECTORY;

CHDDirectory::CHDDirectory()
{}

CHDDirectory::~CHDDirectory()
{}

/////////////////////////////////////////////////////////////////////////////
bool CHDDirectory::GetDirectory(const CStdString& strPath1, CFileItemList &items)
{
  WIN32_FIND_DATA wfd;
  
  CStdString strPath=strPath1;
#ifndef _LINUX
  g_charsetConverter.utf8ToStringCharset(strPath);
#endif

  CFileItemList vecCacheItems;
  g_directoryCache.ClearDirectory(strPath1);

  CStdString strRoot = strPath;
  CURL url(strPath);

  memset(&wfd, 0, sizeof(wfd));
  if (!CUtil::HasSlashAtEnd(strPath) )
#ifndef _LINUX  
    strRoot += "\\";
  strRoot.Replace("/", "\\");
#else
    strRoot += "/";
#endif
  if (CUtil::IsDVD(strRoot) && m_isoReader.IsScanned())
  {
    // Reset iso reader and remount or
    // we can't access the dvd-rom
    m_isoReader.Reset();

    CIoSupport::Dismount("Cdrom0");
    CIoSupport::RemapDriveLetter('D', "Cdrom0");
  }

  CStdString strSearchMask = strRoot;
#ifndef _LINUX
  strSearchMask += "*.*";
#else
  strSearchMask += "*";
#endif

  CAutoPtrFind hFind(FindFirstFile(strSearchMask.c_str(), &wfd));
  
  // On error, check if path exists at all, this will return true if empty folder.
  if (hFind.isValid() == false) 
      return Exists(strPath1);

  do
  {
    if (wfd.cFileName[0] != 0)
    {
      CFileItem* pItem = BuildResolvedFileItem(strRoot, wfd);
      if (pItem)
      {
        // Always add to the cache.
        vecCacheItems.Add(pItem);
      
        // If it's allowed, add it to the list.
        if (IsAllowed(pItem, wfd))
          items.Add(new CFileItem(*pItem));
      }
    }
  }
  while (FindNextFile((HANDLE)hFind, &wfd));
  
#ifdef _XBOX
  // if we use AutoPtrHandle, this auto-closes
  FindClose((HANDLE)hFind); //should be closed
#endif

  if (m_cacheDirectory)
    g_directoryCache.SetDirectory(strPath1, vecCacheItems);
  return true;
}

/////////////////////////////////////////////////////////////////////////////
CFileItem* CHDDirectory::BuildFileItem(const CStdString& strRoot, WIN32_FIND_DATA& wfd)
{
  CStdString strLabel = wfd.cFileName;
#ifndef _LINUX
  g_charsetConverter.stringCharsetToUtf8(strLabel);
#endif

  CFileItem *pItem = new CFileItem(strLabel);
  pItem->m_strPath = strRoot;
  pItem->m_strPath += wfd.cFileName;

#ifndef _LINUX
  g_charsetConverter.stringCharsetToUtf8(pItem->m_strPath);
#endif
  
  if (wfd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
  {
    pItem->m_bIsFolder = true;
    CUtil::AddSlashAtEnd(pItem->m_strPath);
  }
  else
  {
    pItem->m_bIsFolder = false;
    pItem->m_dwSize = CUtil::ToInt64(wfd.nFileSizeHigh, wfd.nFileSizeLow);
  }
  
  return pItem;
}

/////////////////////////////////////////////////////////////////////////////
bool CHDDirectory::IsAllowed(CFileItem* pItem, WIN32_FIND_DATA& wfd)
{
  bool isAllowed = true;
  
#ifdef _LINUX
  if ((wfd.dwFileAttributes & FILE_ATTRIBUTE_HIDDEN) && g_guiSettings.GetBool("filelists.showhidden") == false)
    isAllowed = false;
#endif
  
  if (isAllowed == true)
  {
    if (pItem->m_bIsFolder)
    {
      CStdString strDir = wfd.cFileName;
      if (strDir == "." || strDir == "..")
        isAllowed = false;
    }
    else
    {
      // Make sure to use the resolved file.
      CStdString strFile = CUtil::GetFileName(pItem->m_strPath);
      if (IDirectory::IsAllowed(strFile) == false)
        isAllowed = false;
    }
  }
  
  return isAllowed;
}

/////////////////////////////////////////////////////////////////////////////
CFileItem* CHDDirectory::BuildResolvedFileItem(const CStdString& strRoot, WIN32_FIND_DATA& wfd)
{
  CFileItem* pItem = 0;
  
#ifdef __APPLE__
  // Attempt to resolve aliases.
  FSRef   fsRef;
  Boolean isDir;
  Boolean isAlias;
  char    resolvedAliasPath[MAX_PATH];
  bool    useAlias = false;
              
  // Convert to FSRef.
  CStdString strPath = strRoot;
  CStdString strFile = strPath + wfd.cFileName;
  if (FSPathMakeRef((const UInt8* )strFile.c_str(), &fsRef, &isDir) == noErr)
  {
    if (FSResolveAliasFileWithMountFlags(&fsRef, TRUE, &isDir, &isAlias, kResolveAliasFileNoUI) == noErr)
    {
      // If it was an alias, use the reference instead.
      if (isAlias)
      {
        if (FSRefMakePath(&fsRef, (UInt8* )resolvedAliasPath, MAX_PATH) == noErr)
          useAlias = true;
      }
    }
    else
    {
      // Broken item.
      return 0;
    }
  }
  
  // Compute the *final* name/path of the file.
  if (useAlias)
  {
    strPath = resolvedAliasPath;
    strFile = CUtil::GetFileName(strPath);
    strPath = strPath.Left(strPath.length()-strFile.length());
  }
  else
  {
    strFile = wfd.cFileName;
  }
  
  // Check for smart folders.
  if (CUtil::IsSmartFolder(strFile))
  {
    // Use the original name, without extension.
    CStdString smartFolder = wfd.cFileName;
    int iPos = smartFolder.ReverseFind(".");
    if (iPos > 0)
      smartFolder = smartFolder.Left(iPos);
    
    pItem = new CFileItem(smartFolder);
    pItem->m_strPath = "smartfolder:/" + strPath + strFile;
    pItem->m_bIsFolder = true;
  }
  else if (useAlias)
  {
    // Add the alias.
    pItem = new CFileItem(wfd.cFileName);
    pItem->m_strPath = resolvedAliasPath;
    pItem->m_bIsFolder = isDir ? true : false;
    
    if (isDir == false)
    {
      // Get the size of the file.
      struct stat64 statInfo;
      stat64(resolvedAliasPath, &statInfo);
      pItem->m_dwSize = statInfo.st_size;
    }
  }
  else
#endif
  {
    // Go the default route.
    pItem = BuildFileItem(strRoot, wfd);
  }
  
  // Save file time.
  FILETIME localTime;
  FileTimeToLocalFileTime(&wfd.ftLastWriteTime, &localTime);
  pItem->m_dateTime = localTime;
  
  return pItem;
}

bool CHDDirectory::Create(const char* strPath)
{
  CStdString strPath1 = strPath;
#ifndef _LINUX
  g_charsetConverter.utf8ToStringCharset(strPath1);
#endif
  if (!CUtil::HasSlashAtEnd(strPath1))
#ifndef _LINUX  
    strPath1 += '\\';
#else
    strPath1 += '/';
#endif

#ifdef HAS_FTP_SERVER
  // okey this is really evil, since the create will succed
  // caller have no idea that a different directory was created
  if (g_guiSettings.GetBool("servers.ftpautofatx"))
  {
    CStdString strPath2(strPath1);
    CUtil::GetFatXQualifiedPath(strPath1);
    if(strPath2 != strPath1)
      CLog::Log(LOGNOTICE,"fatxq: %s -> %s",strPath2.c_str(), strPath1.c_str());
  }
#endif

  if(::CreateDirectory(strPath1.c_str(), NULL))
    return true;
  else if(GetLastError() == ERROR_ALREADY_EXISTS)
    return true;

  return false;
}

bool CHDDirectory::Remove(const char* strPath)
{
  CStdString strPath1 = strPath;
#ifndef _LINUX
  g_charsetConverter.utf8ToStringCharset(strPath1);
#endif
  return ::RemoveDirectory(strPath1) ? true : false;
}

bool CHDDirectory::Exists(const char* strPath)
{
  CStdString strReplaced = strPath;
#ifndef _LINUX
  g_charsetConverter.utf8ToStringCharset(strReplaced);
  strReplaced.Replace("/","\\");
#ifdef HAS_XBOX_HARDWARE
  CUtil::GetFatXQualifiedPath(strReplaced);
#endif
  if (!CUtil::HasSlashAtEnd(strReplaced))
    strReplaced += '\\';
#endif    
  DWORD attributes = GetFileAttributes(strReplaced.c_str());
  if(attributes == INVALID_FILE_ATTRIBUTES)
    return false;
  if (FILE_ATTRIBUTE_DIRECTORY & attributes) return true;
  return false;
}
