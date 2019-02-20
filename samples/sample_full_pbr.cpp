/*
 * Copyright (C) 2018 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "app/Config.h"
#include "app/FilamentApp.h"
#include "app/MeshAssimp.h"

#include <filament/Engine.h>
#include <filament/LightManager.h>
#include <filament/Material.h>
#include <filament/MaterialInstance.h>
#include <filament/RenderableManager.h>
#include <filament/TransformManager.h>
#include <filament/Scene.h>
#include <filament/Texture.h>

#include <filamat/MaterialBuilder.h>

#include <utils/Path.h>
#include <utils/EntityManager.h>

#include <math/mat3.h>
#include <math/mat4.h>
#include <math/vec3.h>

#include <getopt/getopt.h>

#include <stb_image.h>

#include <memory>
#include <map>
#include <string>
#include <vector>

using namespace filament::math;
using namespace filament;
using namespace filamat;
using namespace utils;

static std::vector<Path> g_filenames;

static std::map<std::string, MaterialInstance*> g_materialInstances;
static std::unique_ptr<MeshAssimp> g_meshSet;
static const Material* g_material;
static Entity g_light;
static Texture* g_metallicMap = nullptr;
static Texture* g_roughnessMap = nullptr;
static Texture* g_aoMap = nullptr;
static Texture* g_normalMap = nullptr;
static Texture* g_baseColorMap = nullptr;

static Config g_config;
static struct PbrConfig {
    std::string materialDir;
    bool clearCoat = false;
    bool anisotropy = false;
} g_pbrConfig;

static void printUsage(char* name) {
    std::string exec_name(Path(name).getName());
    std::string usage(
            "SAMPLE_PBR is an example of loading PBR assets with base color + packed metallic/roughness\n"
            "Usage:\n"
            "    SAMPLE_PBR [options] <OBJ/FBX/COLLADA>\n"
            "Options:\n"
            "   --help, -h\n"
            "       Prints this message\n\n"
            "   --ibl=<path to cmgen IBL>, -i <path>\n"
            "       Applies an IBL generated by cmgen's deploy option\n\n"
            "   --split-view, -v\n"
            "       Splits the window into 4 views\n\n"
            "   --scale=[number], -s [number]\n"
            "       Applies uniform scale\n\n"
            "   --material=<path>, -m <path>\n"
            "       Directory containing the textures named <DIR_NAME>_*.png where * is:\n"
            "           - AO\n"
            "           - Color\n"
            "           - Metallic\n"
            "           - Normal\n"
            "           - Roughness\n"
            "\n"
            "   --clear-coat, -c\n"
            "       Add a clear coat layer to the material\n\n"
            "   --anisotropy, -a\n"
            "       Enable anisotropy on the material\n\n"
    );
    const std::string from("SAMPLE_PBR");
    for (size_t pos = usage.find(from); pos != std::string::npos; pos = usage.find(from, pos)) {
        usage.replace(pos, from.length(), exec_name);
    }
    std::cout << usage;
}

static int handleCommandLineArgments(int argc, char* argv[], Config* config) {
    static constexpr const char* OPTSTR = "hi:vs:m:ca";
    static const struct option OPTIONS[] = {
            { "help",           no_argument,       nullptr, 'h' },
            { "ibl",            required_argument, nullptr, 'i' },
            { "split-view",     no_argument,       nullptr, 'v' },
            { "scale",          required_argument, nullptr, 's' },
            { "material",       required_argument, nullptr, 'm' },
            { "clear-coat",     no_argument,       nullptr, 'c' },
            { "anisotropy",     no_argument,       nullptr, 'a' },
            { 0, 0, 0, 0 }  // termination of the option list
    };
    int opt;
    int option_index = 0;
    while ((opt = getopt_long(argc, argv, OPTSTR, OPTIONS, &option_index)) >= 0) {
        std::string arg(optarg ? optarg : "");
        switch (opt) {
            default:
            case 'h':
                printUsage(argv[0]);
                exit(0);
            case 'i':
                config->iblDirectory = arg;
                break;
            case 's':
                try {
                    config->scale = std::stof(arg);
                } catch (std::invalid_argument& e) {
                    // keep scale of 1.0
                } catch (std::out_of_range& e) {
                    // keep scale of 1.0
                }
                break;
            case 'v':
                config->splitView = true;
                break;
            case 'm':
                g_pbrConfig.materialDir = arg;
                break;
            case 'c':
                g_pbrConfig.clearCoat = true;
                break;
            case 'a':
                g_pbrConfig.anisotropy = true;
                break;
        }
    }

    return optind;
}

static void cleanup(Engine* engine, View* view, Scene* scene) {
    for (auto& item : g_materialInstances) {
        auto materialInstance = item.second;
        engine->destroy(materialInstance);
    }
    g_meshSet.reset(nullptr);
    engine->destroy(g_material);
    engine->destroy(g_baseColorMap);
    engine->destroy(g_metallicMap);
    engine->destroy(g_roughnessMap);
    engine->destroy(g_aoMap);
    engine->destroy(g_normalMap);

    EntityManager& em = EntityManager::get();
    engine->destroy(g_light);
    em.destroy(g_light);
}

void loadTexture(Engine* engine, const std::string& filePath, Texture** map, bool sRGB = true) {
    if (!filePath.empty()) {
        Path path(filePath);
        if (path.exists()) {
            int w, h, n;
            unsigned char* data = stbi_load(path.getAbsolutePath().c_str(), &w, &h, &n, 3);
            if (data != nullptr) {
                *map = Texture::Builder()
                        .width(uint32_t(w))
                        .height(uint32_t(h))
                        .levels(0xff)
                        .format(sRGB ? driver::TextureFormat::SRGB8 : driver::TextureFormat::RGB8)
                        .build(*engine);
                Texture::PixelBufferDescriptor buffer(data, size_t(w * h * 3),
                        Texture::Format::RGB, Texture::Type::UBYTE,
                        (driver::BufferDescriptor::Callback) &stbi_image_free);
                (*map)->setImage(*engine, 0, std::move(buffer));
                (*map)->generateMipmaps(*engine);
            } else {
                std::cout << "The texture " << path << " could not be loaded" << std::endl;
            }
        } else {
            std::cout << "The texture " << path << " does not exist" << std::endl;
        }
    }
}

static void setup(Engine* engine, View* view, Scene* scene) {
    Path path(g_pbrConfig.materialDir);
    std::string name(path.getName());

    loadTexture(engine, path.concat(name + "_Color.png"), &g_baseColorMap);
    loadTexture(engine, path.concat(name + "_Metallic.png"), &g_metallicMap, false);
    loadTexture(engine, path.concat(name + "_Roughness.png"), &g_roughnessMap, false);
    loadTexture(engine, path.concat(name + "_AO.png"), &g_aoMap, false);
    loadTexture(engine, path.concat(name + "_Normal.png"), &g_normalMap, false);

    bool hasBaseColorMap = g_baseColorMap != nullptr;
    bool hasMetallicMap = g_metallicMap != nullptr;
    bool hasRoughnessMap = g_roughnessMap != nullptr;
    bool hasAOMap = g_aoMap != nullptr;
    bool hasNormalMap = g_normalMap != nullptr;

    std::string shader = R"SHADER(
        void material(inout MaterialInputs material) {
    )SHADER";

    if (hasNormalMap) {
        shader += R"SHADER(
            material.normal = texture(materialParams_normalMap, getUV0()).xyz * 2.0 - 1.0;
            material.normal.y *= -1.0;
        )SHADER";
    }

    shader += R"SHADER(
        prepareMaterial(material);
    )SHADER";

    if (hasBaseColorMap) {
        shader += R"SHADER(
            material.baseColor.rgb = texture(materialParams_baseColorMap, getUV0()).rgb;
        )SHADER";
    } else {
        shader += R"SHADER(
            material.baseColor.rgb = float3(1.0, 0.75, 0.94);
        )SHADER";
    }
    if (hasMetallicMap) {
        shader += R"SHADER(
            material.metallic = texture(materialParams_metallicMap, getUV0()).r;
        )SHADER";
    } else {
        shader += R"SHADER(
            material.metallic = 0.0;
        )SHADER";
    }
    if (hasRoughnessMap) {
        shader += R"SHADER(
            material.roughness = texture(materialParams_roughnessMap, getUV0()).r;
        )SHADER";
    } else {
        shader += R"SHADER(
            material.roughness = 1.0;
        )SHADER";
    }
    if (hasAOMap) {
        shader += R"SHADER(
            material.ambientOcclusion = texture(materialParams_aoMap, getUV0()).r;
        )SHADER";
    } else {
        shader += R"SHADER(
            material.ambientOcclusion = 1.0;
        )SHADER";
    }

    if (g_pbrConfig.clearCoat) {
        shader += R"SHADER(
            material.clearCoat = 1.0;
        )SHADER";
    }
    if (g_pbrConfig.anisotropy) {
        shader += R"SHADER(
            material.anisotropy = 0.7;
        )SHADER";
    }
    shader += "}\n";

    MaterialBuilder::init();
    MaterialBuilder builder = MaterialBuilder()
            .name("DefaultMaterial")
            .material(shader.c_str())
            .shading(Shading::LIT);

    if (hasBaseColorMap) {
        builder
            .require(VertexAttribute::UV0)
            .parameter(MaterialBuilder::SamplerType::SAMPLER_2D, "baseColorMap");
    }
    if (hasMetallicMap) {
        builder
            .require(VertexAttribute::UV0)
            .parameter(MaterialBuilder::SamplerType::SAMPLER_2D, "metallicMap");
    }
    if (hasRoughnessMap) {
        builder
            .require(VertexAttribute::UV0)
            .parameter(MaterialBuilder::SamplerType::SAMPLER_2D, "roughnessMap");
    }
    if (hasAOMap) {
        builder
            .require(VertexAttribute::UV0)
            .parameter(MaterialBuilder::SamplerType::SAMPLER_2D, "aoMap");
    }
    if (hasNormalMap) {
        builder
            .require(VertexAttribute::UV0)
            .parameter(MaterialBuilder::SamplerType::SAMPLER_2D, "normalMap");
    }

    Package pkg = builder.build();

    g_material = Material::Builder().package(pkg.getData(), pkg.getSize()).build(*engine);
    g_materialInstances["DefaultMaterial"] = g_material->createInstance();

    TextureSampler sampler(TextureSampler::MinFilter::LINEAR_MIPMAP_LINEAR,
            TextureSampler::MagFilter::LINEAR, TextureSampler::WrapMode::REPEAT);
    sampler.setAnisotropy(8.0f);

    if (hasBaseColorMap) {
        g_materialInstances["DefaultMaterial"]->setParameter(
                "baseColorMap", g_baseColorMap, sampler);
    }
    if (hasMetallicMap) {
        g_materialInstances["DefaultMaterial"]->setParameter(
                "metallicMap", g_metallicMap, sampler);
    }
    if (hasRoughnessMap) {
        g_materialInstances["DefaultMaterial"]->setParameter(
                "roughnessMap", g_roughnessMap, sampler);
    }
    if (hasAOMap) {
        g_materialInstances["DefaultMaterial"]->setParameter(
                "aoMap", g_aoMap, sampler);
    }
    if (hasNormalMap) {
        g_materialInstances["DefaultMaterial"]->setParameter(
                "normalMap", g_normalMap, sampler);
    }

    g_meshSet = std::make_unique<MeshAssimp>(*engine);
    for (auto& filename : g_filenames) {
        g_meshSet->addFromFile(filename, g_materialInstances, true);
    }

    auto& rcm = engine->getRenderableManager();
    auto& tcm = engine->getTransformManager();
    for (auto renderable : g_meshSet->getRenderables()) {
        if (rcm.hasComponent(renderable)) {
            auto ti = tcm.getInstance(renderable);
            tcm.setTransform(ti, mat4f{ mat3f(g_config.scale), float3(0.0f, 0.0f, -4.0f) } *
                    tcm.getWorldTransform(ti));
            scene->addEntity(renderable);
        }
    }

    g_light = EntityManager::get().create();
    LightManager::Builder(LightManager::Type::DIRECTIONAL)
            .color(Color::toLinear<ACCURATE>({0.98f, 0.92f, 0.89f}))
            .intensity(110000)
            .direction({0.6, -1, -0.8})
            .build(*engine, g_light);
    scene->addEntity(g_light);
}

int main(int argc, char* argv[]) {
    int option_index = handleCommandLineArgments(argc, argv, &g_config);
    int num_args = argc - option_index;
    if (num_args < 1) {
        printUsage(argv[0]);
        return 1;
    }

    for (int i = option_index; i < argc; i++) {
        utils::Path filename = argv[i];
        if (!filename.exists()) {
            std::cerr << "file " << argv[i] << " not found!" << std::endl;
            return 1;
        }
        g_filenames.push_back(filename);
    }

    g_config.title = "PBR";
    FilamentApp& filamentApp = FilamentApp::get();
    filamentApp.run(g_config, setup, cleanup);

    return 0;
}
