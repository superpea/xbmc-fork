#include "stdafx.h"
#include "CocoaUtils.h"
#include "FileItem.h"
#include "SmartFolderDirectory.h"
#include "Util.h"

using namespace DIRECTORY;

bool CSmartFolderDirectory::GetDirectory(const CStdString& strPath, CFileItemList &items)
{
  CStdString strTruePath = strPath.Right(strPath.length()-13).c_str();
  
  // If we don't have a savedSearch at the end, we've gone up.
  if (CUtil::GetExtension(strTruePath) != ".savedSearch")
    return CHDDirectory::GetDirectory(strTruePath, items);
  else
    Cocoa_GetSmartFolderResults(strTruePath, HandleSearchResult, this, &items);
  
  return true;
}

void CSmartFolderDirectory::HandleSearchResult(void* thisPtr, void* itemListPtr, const char* strFilePath)
{
  CSmartFolderDirectory* me = (CSmartFolderDirectory* )thisPtr;
  CFileItemList* pItemList = (CFileItemList* )itemListPtr;
  
  // Load up a find data.
  WIN32_FIND_DATA wfd;
  memset(&wfd, 0, sizeof(WIN32_FIND_DATA));

  // Filename.
  CStdString strPath = strFilePath;
  CStdString strFile = CUtil::GetFileName(strPath);
  
  strPath = strPath.Left(strPath.length()-strFile.length());
  strcpy(wfd.cFileName, strFile.c_str());
  
  // Get the attributes.
  wfd.dwFileAttributes = FILE_ATTRIBUTE_NORMAL;

  // Is it a directory?
  if (strPath[0] == '.')
    wfd.dwFileAttributes |= FILE_ATTRIBUTE_HIDDEN;

  struct stat64 fileStat;
  if (stat64(strFilePath, &fileStat) != 0)
    return;
  
  if (S_ISDIR(fileStat.st_mode))
    wfd.dwFileAttributes |= FILE_ATTRIBUTE_DIRECTORY;
  
  wfd.nFileSizeHigh = (DWORD)(fileStat.st_size >> 32);
  wfd.nFileSizeLow  = (DWORD)fileStat.st_size;
  TimeTToFileTime(fileStat.st_ctime, &wfd.ftCreationTime);
  TimeTToFileTime(fileStat.st_atime, &wfd.ftLastAccessTime);
  TimeTToFileTime(fileStat.st_mtime, &wfd.ftLastWriteTime);

  CFileItem* pItem = me->BuildResolvedFileItem(strPath, wfd);
        
  // If it's allowed, add it to the list.
  if (pItem && me->IsAllowed(pItem, wfd))
    pItemList->Add(new CFileItem(*pItem));
}
