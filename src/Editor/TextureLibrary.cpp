#include "Editor/TextureLibrary.h"

#include "AssetPaths.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <iostream>

namespace editor
{

namespace
{
std::string toLower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}
} // namespace

bool TextureLibrary::isTextureExtension(const std::string& lowerExtension)
{
    return lowerExtension == ".png" || lowerExtension == ".jpg" || lowerExtension == ".jpeg" ||
           lowerExtension == ".tga" || lowerExtension == ".bmp";
}

void TextureLibrary::refresh()
{
    std::vector<TextureEntry> scanned;

    const std::filesystem::path root = std::filesystem::path(TINY_RESOURCE_DIR) / "Texture";
    rootPath = root.string();

    std::error_code ec;
    if (!std::filesystem::exists(root, ec))
    {
        std::cerr << "[Textures] directory not found: " << rootPath << std::endl;
        items.clear();
        return;
    }

    for (const auto& entry : std::filesystem::recursive_directory_iterator(root, ec))
    {
        if (ec)
            break;
        if (!entry.is_regular_file())
            continue;

        const std::string ext = toLower(entry.path().extension().string());
        if (!isTextureExtension(ext))
            continue;

        TextureEntry item;
        item.absolutePath = entry.path().string();

        std::error_code relEc;
        const auto rel = std::filesystem::relative(entry.path(), root, relEc);
        item.displayName = relEc ? entry.path().filename().string() : rel.string();
        scanned.push_back(std::move(item));
    }

    std::sort(scanned.begin(), scanned.end(), [](const TextureEntry& a, const TextureEntry& b)
    {
        return a.displayName < b.displayName;
    });

    items = std::move(scanned);
    std::cout << "[Textures] scanned " << items.size() << " texture file(s) under " << rootPath
              << std::endl;
}

bool TextureLibrary::selfTest()
{
    const bool accepts = isTextureExtension(".png") && isTextureExtension(".jpg") &&
                         isTextureExtension(".jpeg") && isTextureExtension(".tga") &&
                         isTextureExtension(".bmp");
    const bool rejects = !isTextureExtension(".hdr") && !isTextureExtension(".gltf") &&
                         !isTextureExtension(".ktx2") && !isTextureExtension("");
    if (!accepts || !rejects)
    {
        std::cerr << "[Editor] texture library selfTest FAIL | accepts=" << accepts
                  << " rejects=" << rejects << std::endl;
        return false;
    }
    std::cout << "[Editor] texture library selfTest PASS | png/jpg/jpeg/tga/bmp only, hdr needs its own path"
              << std::endl;
    return true;
}

} // namespace editor
