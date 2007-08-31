#pragma once
#include "DirectoryNode.h"

namespace DIRECTORY
{
  namespace VIDEODATABASEDIRECTORY
  {
    class CDirectoryNodeRecentlyAddedMovies : public CDirectoryNode
    {
    public:
      CDirectoryNodeRecentlyAddedMovies(const CStdString& strEntryName, CDirectoryNode* pParent);
    protected:
      virtual bool GetContent(CFileItemList& items);
      virtual NODE_TYPE GetChildType();
    };
  };
};
