/*
Open Asset Import Library (assimp)
----------------------------------------------------------------------

Copyright (c) 2006-2020, assimp team

All rights reserved.

Redistribution and use of this software in source and binary forms,
with or without modification, are permitted provided that the
following conditions are met:

* Redistributions of source code must retain the above
  copyright notice, this list of conditions and the
  following disclaimer.

* Redistributions in binary form must reproduce the above
  copyright notice, this list of conditions and the
  following disclaimer in the documentation and/or other
  materials provided with the distribution.

* Neither the name of the assimp team, nor the names of its
  contributors may be used to endorse or promote products
  derived from this software without specific prior
  written permission of the assimp team.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
"AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

----------------------------------------------------------------------
*/

#ifndef ASSIMP_BUILD_NO_3MF_IMPORTER

#include "D3MFImporter.h"

#include <assimp/StringComparison.h>
#include <assimp/StringUtils.h>
#include <assimp/XmlParser.h>
#include <assimp/ZipArchiveIOSystem.h>
#include <assimp/importerdesc.h>
#include <assimp/scene.h>
#include <assimp/DefaultLogger.hpp>
#include <assimp/IOSystem.hpp>
#include <cassert>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "3MFXmlTags.h"
#include "D3MFOpcPackage.h"
#include <assimp/fast_atof.h>

#include <iomanip>

namespace Assimp {
namespace D3MF {

class XmlSerializer {
public:

    XmlSerializer(XmlParser *xmlParser) :
            mMeshes(),
            mBasematerialsDictionnary(),
            mMaterialCount(0),
            mXmlParser(xmlParser) {
        // empty
    }

    ~XmlSerializer() {
        // empty
    }

    void ImportXml(aiScene *scene) {
        if (nullptr == scene) {
            return;
        }

        scene->mRootNode = new aiNode();
        std::vector<aiNode *> children;

        std::string nodeName;
        XmlNode node = mXmlParser->getRootNode().child("model");
        if (node.empty()) {
            return;
        }
        XmlNode resNode = node.child("resources");
        for (XmlNode currentNode = resNode.first_child(); currentNode; currentNode = currentNode.next_sibling()) {
            const std::string &currentNodeName = currentNode.name();
            if (currentNodeName == D3MF::XmlTag::object) {
                children.push_back(ReadObject(currentNode, scene));
            } else if (currentNodeName == D3MF::XmlTag::build) {
                //
            } else if (currentNodeName == D3MF::XmlTag::basematerials) {
                ReadBaseMaterials(currentNode);
            } else if (currentNodeName == D3MF::XmlTag::meta) {
                ReadMetadata(currentNode);
            }
        }

        if (scene->mRootNode->mName.length == 0) {
            scene->mRootNode->mName.Set("3MF");
        }

        // import the metadata
        if (!mMetaData.empty()) {
            const size_t numMeta(mMetaData.size());
            scene->mMetaData = aiMetadata::Alloc(static_cast<unsigned int>(numMeta));
            for (size_t i = 0; i < numMeta; ++i) {
                aiString val(mMetaData[i].value);
                scene->mMetaData->Set(static_cast<unsigned int>(i), mMetaData[i].name, val);
            }
        }

        // import the meshes
        scene->mNumMeshes = static_cast<unsigned int>(mMeshes.size());
        scene->mMeshes = new aiMesh *[scene->mNumMeshes]();
        std::copy(mMeshes.begin(), mMeshes.end(), scene->mMeshes);

        // import the materials
        scene->mNumMaterials = static_cast<unsigned int>(mMaterialCount);
        if (0 != scene->mNumMaterials) {
            scene->mMaterials = new aiMaterial *[scene->mNumMaterials];
            for (auto it = mBasematerialsDictionnary.begin(); it != mBasematerialsDictionnary.end(); it++) {
                for (unsigned int i = 0; i < it->second.size(); ++i) {
                    scene->mMaterials[it->second[i].first] = it->second[i].second;
                }
            }
        }

        // create the scene-graph
        scene->mRootNode->mNumChildren = static_cast<unsigned int>(children.size());
        scene->mRootNode->mChildren = new aiNode *[scene->mRootNode->mNumChildren]();
        std::copy(children.begin(), children.end(), scene->mRootNode->mChildren);
    }

private:

    bool getNodeAttribute(const XmlNode& node, const std::string& attribute, std::string& value) {
        pugi::xml_attribute objectAttribute = node.attribute(attribute.c_str());
        if (!objectAttribute.empty()) {
            value = objectAttribute.as_string();
            return true;
        } else {
            return false;
        }
    }

    aiNode *ReadObject(XmlNode &node, aiScene *scene) {
        std::unique_ptr<aiNode> nodePtr(new aiNode());

        std::vector<unsigned long> meshIds;

        std::string id, type, pid, pindex;
        bool hasId = getNodeAttribute(node, D3MF::XmlTag::id, id);
        //bool hasType = getNodeAttribute(node, D3MF::XmlTag::type, type); not used currently
        bool hasPid = getNodeAttribute(node, D3MF::XmlTag::pid, pid);
        bool hasPindex = getNodeAttribute(node, D3MF::XmlTag::pindex, pindex);

        if (!hasId) {
            return nullptr;
        }

        nodePtr->mParent = scene->mRootNode;
        nodePtr->mName.Set(id);

        size_t meshIdx = mMeshes.size();

        for (XmlNode currentNode = node.first_child(); currentNode; currentNode = currentNode.next_sibling()) {
            const std::string &currentName = currentNode.name();
            if (currentName == D3MF::XmlTag::mesh) {
                auto mesh = ReadMesh(currentNode);
                mesh->mName.Set(id);
                if (hasPid && hasPindex && mBasematerialsDictionnary.find(atoi(pid.c_str())) != mBasematerialsDictionnary.end()) {
                    int iPid = atoi(pid.c_str());
                    int iPindex = atoi(pindex.c_str());
                    mesh->mMaterialIndex = mBasematerialsDictionnary[iPid][iPindex].first;
                    std::cout << "Set material " << mesh->mMaterialIndex << " from pid " << iPid << " and pindex " << iPindex << std::endl;
                }
                mMeshes.push_back(mesh);
                meshIds.push_back(static_cast<unsigned long>(meshIdx));
                ++meshIdx;
            }
        }

        nodePtr->mNumMeshes = static_cast<unsigned int>(meshIds.size());

        nodePtr->mMeshes = new unsigned int[nodePtr->mNumMeshes];

        std::copy(meshIds.begin(), meshIds.end(), nodePtr->mMeshes);

        return nodePtr.release();
    }

    aiMesh *ReadMesh(XmlNode &node) {
        aiMesh *mesh = new aiMesh();
        for (XmlNode currentNode = node.first_child(); currentNode; currentNode = currentNode.next_sibling()) {
            const std::string &currentName = currentNode.name();
            if (currentName == D3MF::XmlTag::vertices) {
                ImportVertices(currentNode, mesh);
            } else if (currentName == D3MF::XmlTag::triangles) {
                ImportTriangles(currentNode, mesh);
            }

        }

        return mesh;
    }

    void ReadMetadata(XmlNode &node) {
        pugi::xml_attribute attribute = node.attribute(D3MF::XmlTag::meta_name.c_str());
        const std::string name = attribute.as_string();
        const std::string value = node.value();
        if (name.empty()) {
            return;
        }

        MetaEntry entry;
        entry.name = name;
        entry.value = value;
        mMetaData.push_back(entry);
    }

    void ImportVertices(XmlNode &node, aiMesh *mesh) {
        std::vector<aiVector3D> vertices;
        for (XmlNode currentNode = node.first_child(); currentNode; currentNode = currentNode.next_sibling()) {
            const std::string &currentName = currentNode.name();
            if (currentName == D3MF::XmlTag::vertex) {
                vertices.push_back(ReadVertex(currentNode));
            }
        }

        mesh->mNumVertices = static_cast<unsigned int>(vertices.size());
        mesh->mVertices = new aiVector3D[mesh->mNumVertices];
        std::copy(vertices.begin(), vertices.end(), mesh->mVertices);
    }

    aiVector3D ReadVertex(XmlNode &node) {
        aiVector3D vertex;
        vertex.x = ai_strtof(node.attribute(D3MF::XmlTag::x.c_str()).as_string(), nullptr);
        vertex.y = ai_strtof(node.attribute(D3MF::XmlTag::y.c_str()).as_string(), nullptr);
        vertex.z = ai_strtof(node.attribute(D3MF::XmlTag::z.c_str()).as_string(), nullptr);

        return vertex;
    }

    void ImportTriangles(XmlNode &node, aiMesh *mesh) {
        std::vector<aiFace> faces;
        for (XmlNode currentNode = node.first_child(); currentNode; currentNode = currentNode.next_sibling()) {
            const std::string &currentName = currentNode.name();
            if (currentName == D3MF::XmlTag::triangle) {
                faces.push_back(ReadTriangle(currentNode));
                const char *pidToken = currentNode.attribute(D3MF::XmlTag::pid.c_str()).as_string();
                const char *p1Token = currentNode.attribute(D3MF::XmlTag::p1.c_str()).as_string();
                if (nullptr != pidToken && nullptr != p1Token && mBasematerialsDictionnary.find(std::atoi(pidToken)) != mBasematerialsDictionnary.end()) {
                    int pid(std::atoi(pidToken));
                    int p1(std::atoi(p1Token));
                    mesh->mMaterialIndex = mBasematerialsDictionnary[pid][p1].first;
                    // TODO: manage the separation into several meshes if the triangles of the mesh do not all refer to the same material
                }
            }
        }

        mesh->mNumFaces = static_cast<unsigned int>(faces.size());
        mesh->mFaces = new aiFace[mesh->mNumFaces];
        mesh->mPrimitiveTypes = aiPrimitiveType_TRIANGLE;

        std::copy(faces.begin(), faces.end(), mesh->mFaces);
    }

    aiFace ReadTriangle(XmlNode &node) {
        aiFace face;

        face.mNumIndices = 3;
        face.mIndices = new unsigned int[face.mNumIndices];
        face.mIndices[0] = static_cast<unsigned int>(std::atoi(node.attribute(D3MF::XmlTag::v1.c_str()).as_string()));
        face.mIndices[1] = static_cast<unsigned int>(std::atoi(node.attribute(D3MF::XmlTag::v2.c_str()).as_string()));
        face.mIndices[2] = static_cast<unsigned int>(std::atoi(node.attribute(D3MF::XmlTag::v3.c_str()).as_string()));

        return face;
    }

    void ReadBaseMaterials(XmlNode &node) {
        std::vector<unsigned int> MatIdArray;
        const char *baseMaterialId = node.attribute(D3MF::XmlTag::basematerials_id.c_str()).as_string();
        if (nullptr != baseMaterialId) {
            unsigned int id = std::atoi(baseMaterialId);
            std::vector<std::pair<unsigned int, aiMaterial *> > materials;

            for (XmlNode currentNode = node.first_child(); currentNode; currentNode = currentNode.next_sibling())
            {
                if (currentNode.name() == D3MF::XmlTag::basematerials_base) {
                    materials.push_back(std::make_pair(mMaterialCount, readMaterialDef(currentNode, id)));
                    mMaterialCount++;
                }
            }

            mBasematerialsDictionnary.insert(std::make_pair(id, materials));
        }
    }

    bool parseColor(const char *color, aiColor4D &diffuse) {
        if (nullptr == color) {
            return false;
        }

        //format of the color string: #RRGGBBAA or #RRGGBB (3MF Core chapter 5.1.1)
        const size_t len(strlen(color));
        if (9 != len && 7 != len) {
            return false;
        }

        const char *buf(color);
        if ('#' != *buf) {
            return false;
        }
        ++buf;
        char comp[3] = { 0, 0, '\0' };

        comp[0] = *buf;
        ++buf;
        comp[1] = *buf;
        ++buf;
        diffuse.r = static_cast<ai_real>(strtol(comp, nullptr, 16)) / ai_real(255.0);

        comp[0] = *buf;
        ++buf;
        comp[1] = *buf;
        ++buf;
        diffuse.g = static_cast<ai_real>(strtol(comp, nullptr, 16)) / ai_real(255.0);

        comp[0] = *buf;
        ++buf;
        comp[1] = *buf;
        ++buf;
        diffuse.b = static_cast<ai_real>(strtol(comp, nullptr, 16)) / ai_real(255.0);

        if (7 == len)
            return true;
        comp[0] = *buf;
        ++buf;
        comp[1] = *buf;
        ++buf;
        diffuse.a = static_cast<ai_real>(strtol(comp, nullptr, 16)) / ai_real(255.0);

        return true;
    }

    void assignDiffuseColor(XmlNode &node, aiMaterial *mat) {
        const char *color = node.attribute(D3MF::XmlTag::basematerials_displaycolor.c_str()).as_string();
        aiColor4D diffuse;
        if (parseColor(color, diffuse)) {
            mat->AddProperty<aiColor4D>(&diffuse, 1, AI_MATKEY_COLOR_DIFFUSE);
        }
    }

    aiMaterial *readMaterialDef(XmlNode &node, unsigned int basematerialsId) {
        aiMaterial *mat(nullptr);
        const char *name(nullptr);
        const std::string nodeName = node.name();
        if (nodeName == D3MF::XmlTag::basematerials_base) {
            name = node.attribute(D3MF::XmlTag::basematerials_name.c_str()).as_string();
            std::string stdMatName;
            aiString matName;
            std::string strId(to_string(basematerialsId));
            stdMatName += "id";
            stdMatName += strId;
            stdMatName += "_";
            if (nullptr != name) {
                stdMatName += std::string(name);
            } else {
                stdMatName += "basemat_";
                stdMatName += to_string(mMaterialCount - basematerialsId);
            }
            matName.Set(stdMatName);

            mat = new aiMaterial;
            mat->AddProperty(&matName, AI_MATKEY_NAME);

            assignDiffuseColor(node, mat);
        }

        return mat;
    }

private:
    struct MetaEntry {
        std::string name;
        std::string value;
    };
    std::vector<MetaEntry> mMetaData;
    std::vector<aiMesh *> mMeshes;
    std::map<unsigned int, std::vector<std::pair<unsigned int, aiMaterial *>>> mBasematerialsDictionnary;
    unsigned int mMaterialCount;
    XmlParser *mXmlParser;
};

} //namespace D3MF

static const aiImporterDesc desc = {
    "3mf Importer",
    "",
    "",
    "http://3mf.io/",
    aiImporterFlags_SupportBinaryFlavour | aiImporterFlags_SupportCompressedFlavour,
    0,
    0,
    0,
    0,
    "3mf"
};

D3MFImporter::D3MFImporter() :
        BaseImporter() {
    // empty
}

D3MFImporter::~D3MFImporter() {
    // empty
}

bool D3MFImporter::CanRead(const std::string &filename, IOSystem *pIOHandler, bool checkSig) const {
    const std::string extension(GetExtension(filename));
    if (extension == desc.mFileExtensions) {
        return true;
    } else if (!extension.length() || checkSig) {
        if (nullptr == pIOHandler) {
            return false;
        }
        if (!ZipArchiveIOSystem::isZipArchive(pIOHandler, filename)) {
            return false;
        }
        D3MF::D3MFOpcPackage opcPackage(pIOHandler, filename);
        return opcPackage.validate();
    }

    return false;
}

void D3MFImporter::SetupProperties(const Importer * /*pImp*/) {
    // empty
}

const aiImporterDesc *D3MFImporter::GetInfo() const {
    return &desc;
}

void D3MFImporter::InternReadFile(const std::string &filename, aiScene *pScene, IOSystem *pIOHandler) {
    D3MF::D3MFOpcPackage opcPackage(pIOHandler, filename);

    XmlParser xmlParser;
    if (xmlParser.parse(opcPackage.RootStream())) {
        D3MF::XmlSerializer xmlSerializer(&xmlParser);
        xmlSerializer.ImportXml(pScene);
    }
}

} // Namespace Assimp

#endif // ASSIMP_BUILD_NO_3MF_IMPORTER
