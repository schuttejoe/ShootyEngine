//==============================================================================
// Joe Schutte
//==============================================================================

#include "BuildCore/BuildContext.h"
#include "IoLib/File.h"
#include "IoLib/Environment.h"
#include "StringLib/StringUtil.h"

#include <stdio.h>

namespace Selas
{
    //==============================================================================
    Error BuildProcessorContext::AddFileDependency(cpointer file)
    {
        ContentDependency dep;

        AssetFileUtils::SanitizeContentPath(file, dep.path);

        if(FileTime(file, &dep.timestamp) == false) {
            return Error_("Failed to find file: %s", file);
        }

        contentDependencies.Add(dep);

        return Success_;
    }

    //==============================================================================
    Error BuildProcessorContext::AddProcessDependency(const ContentId& source)
    {
        ProcessDependency dep;
        dep.source = source;
        dep.id = AssetId(source.type.Ascii(), source.name.Ascii());

         processDependencies.Add(dep);

        return Success_;
    }

    //==============================================================================
    Error BuildProcessorContext::AddProcessDependency(cpointer type, cpointer name)
    {
        return AddProcessDependency(ContentId(type, name));
    }

    //==============================================================================
    //Error BuildProcessorContext::AddBuildDependency(const BuildId& dependee, const BuildId& dependency)
    //{
    //    BuildDependency& dep = dependencies->buildDependencies.Add();
    //    dep.dependeeId   = dependee;
    //    dep.dependencyId = dependency;

    //    return Success_;
    //}

    //==============================================================================
    Error BuildProcessorContext::CreateOutput(cpointer type, uint64 version, cpointer name, const void* data, uint64 dataSize)
    {
        ProcessorOutput output;
        output.source = ContentId(type, name);
        output.id = AssetId(type, name);
        output.version = version;

        FilePathString filepath;
        AssetFileUtils::AssetFilePath(output.id, version, filepath);

        ReturnError_(File::WriteWholeFile(filepath.Ascii(), data, (uint32)dataSize));

        outputs.Add(output);

        return Success_;
    }

    //==============================================================================
    void BuildProcessorContext::Initialize(ContentId source_, AssetId id_)
    {
        source = source_;
        id = id_;

        Assert_(contentDependencies.Length() == 0);
        Assert_(processDependencies.Length() == 0);
        Assert_(outputs.Length() == 0);
    }
}