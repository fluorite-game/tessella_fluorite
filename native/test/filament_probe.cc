// SPDX-License-Identifier: Apache-2.0
//
// The Filament resource path, headless.
//
// Proves what option (a) needs and nothing more: a material compiled ahead by `matc` loads
// through `Material::Builder::package`, instantiates, and takes the buffers a tessella drawable
// actually arrives as -- tile-local `SHORT2` positions and `USHORT` indices -- ending as a
// renderable in a scene.
//
// It runs on `Backend::NOOP`, Filament's no-op driver. That is what makes the whole resource path
// testable with no GPU, no display and no window: everything here except the pixels is exercised,
// and the pixels are the part a device would have to judge anyway.
//
// Prints `name value` lines for the Rust side to read.

#include <filament/Engine.h>
#include <filament/Material.h>
#include <filament/MaterialInstance.h>
#include <filament/VertexBuffer.h>
#include <filament/IndexBuffer.h>
#include <filament/Scene.h>
#include <filament/RenderableManager.h>
#include <utils/EntityManager.h>
#include <cstdio>
#include <cstdlib>
#include <vector>

int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: %s <fill.filamat>\n", argv[0]); return 2; }
    std::FILE* f = std::fopen(argv[1], "rb");
    if (!f) { std::fprintf(stderr, "cannot open\n"); return 2; }
    std::vector<uint8_t> pkg;
    char buf[4096]; size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) pkg.insert(pkg.end(), buf, buf + n);
    std::fclose(f);
    std::printf("package_bytes %zu\n", pkg.size());

    auto* engine = filament::Engine::Builder().backend(filament::backend::Backend::NOOP).build();
    std::printf("engine %d\n", engine != nullptr);
    if (!engine) return 1;

    auto* material = filament::Material::Builder().package(pkg.data(), pkg.size()).build(*engine);
    std::printf("material %d\n", material != nullptr);
    if (!material) { filament::Engine::destroy(&engine); return 1; }

    auto* mi = material->createInstance();
    std::printf("instance %d\n", mi != nullptr);

    // Two triangles' worth of tile-local i16 positions, the shape a fill drawable arrives as.
    struct V { int16_t x, y; };
    std::vector<V> verts = {{0,0},{4096,0},{4096,4096},{0,4096}};
    std::vector<uint16_t> idx = {0,1,2, 0,2,3};

    auto* vb = filament::VertexBuffer::Builder()
        .vertexCount(static_cast<uint32_t>(verts.size()))
        .bufferCount(1)
        .attribute(filament::VertexAttribute::POSITION, 0,
                   filament::VertexBuffer::AttributeType::SHORT2, 0, sizeof(V))
        .normalized(filament::VertexAttribute::POSITION, false)
        .build(*engine);
    std::printf("vertex_buffer %d\n", vb != nullptr);
    if (vb) {
        vb->setBufferAt(*engine, 0, filament::VertexBuffer::BufferDescriptor(
            verts.data(), verts.size() * sizeof(V), nullptr));
    }

    auto* ib = filament::IndexBuffer::Builder()
        .indexCount(static_cast<uint32_t>(idx.size()))
        .bufferType(filament::IndexBuffer::IndexType::USHORT)
        .build(*engine);
    std::printf("index_buffer %d\n", ib != nullptr);
    if (ib) {
        ib->setBuffer(*engine, filament::IndexBuffer::BufferDescriptor(
            idx.data(), idx.size() * sizeof(uint16_t), nullptr));
    }

    auto* scene = engine->createScene();
    auto entity = utils::EntityManager::get().create();
    filament::RenderableManager::Builder(1)
        .boundingBox({{0,0,0},{4096,4096,0}})
        .material(0, mi)
        .geometry(0, filament::RenderableManager::PrimitiveType::TRIANGLES, vb, ib, 0, idx.size())
        .culling(false)
        .build(*engine, entity);
    const bool renderable = engine->getRenderableManager().hasComponent(entity);
    std::printf("renderable %d\n", renderable ? 1 : 0);
    scene->addEntity(entity);
    std::printf("scene_entities %zu\n", scene->getEntityCount());

    engine->destroy(entity);
    utils::EntityManager::get().destroy(entity);
    engine->destroy(ib); engine->destroy(vb); engine->destroy(mi); engine->destroy(material);
    engine->destroy(scene);
    filament::Engine::destroy(&engine);
    std::printf("done 1\n");
    return 0;
}
