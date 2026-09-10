// SPDX-License-Identifier: Apache-2.0
//
// Can one material package serve every data-driven paint permutation?
//
// mbgl compiles a shader per permutation and branches on `HAS_UNIFORM_u_color`. Filament has no
// runtime compiler, and enumerating permutations offline does not scale: the line family alone has
// six data-driven properties -- sixty-four packages -- times the surface variants each family
// already ships. So the question is whether a specialization constant does the same job from one
// package, and whether the drawables that do *not* supply an attribute can still satisfy the
// `requires` list that lets the other ones read it.
//
// Three cases, one material package:
//
//   uniform     constant paint, attribute slot fed by the shared zero buffer
//   attribute   data-driven paint, attribute slot fed by the drawable's own buffer
//   shared      a second constant-paint drawable on the *same* zero buffer object
//
// The third is the one that decides the memory story. If a single `BufferObject` can back the
// unread slot of every constant-paint drawable in the frame, the permutation costs nothing per
// drawable; if each needs its own, the mechanism is paid for per tile per layer.
//
//   permutation_probe <permutation_probe.filamat>

#include <filament/BufferObject.h>
#include <filament/Camera.h>
#include <filament/Engine.h>
#include <filament/IndexBuffer.h>
#include <filament/Material.h>
#include <filament/MaterialInstance.h>
#include <filament/RenderableManager.h>
#include <filament/Renderer.h>
#include <filament/Scene.h>
#include <filament/SwapChain.h>
#include <filament/VertexBuffer.h>
#include <filament/View.h>
#include <filament/Viewport.h>
#include <backend/PixelBufferDescriptor.h>
#include <math/mat4.h>
#include <utils/EntityManager.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

namespace {

constexpr std::uint32_t W = 64;
constexpr std::uint32_t H = 64;

std::string slurp(const char* path) {
    std::ifstream file(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <permutation_probe.filamat>\n", argv[0]);
        return 2;
    }
    const std::string package = slurp(argv[1]);
    if (package.empty()) {
        std::fprintf(stderr, "probe: cannot read %s\n", argv[1]);
        return 2;
    }

    auto* engine = filament::Engine::Builder().backend(filament::Engine::Backend::VULKAN).build();
    auto* swapChain = engine->createSwapChain(W, H, filament::SwapChain::CONFIG_READABLE);
    auto* renderer = engine->createRenderer();
    auto* view = engine->createView();
    auto cameraEntity = utils::EntityManager::get().create();
    auto* camera = engine->createCamera(cameraEntity);
    view->setCamera(camera);
    view->setViewport({0, 0, W, H});
    view->setPostProcessingEnabled(false);
    renderer->setClearOptions({.clearColor = {0.0f, 0.0f, 0.0f, 1.0f}, .clear = true});

    // Two materials from one package. `constant()` is resolved at build, so each is a distinct
    // program and neither carries the other's branch.
    auto* uniformMaterial = filament::Material::Builder()
                                .package(package.data(), package.size())
                                .constant("colorFromAttribute", false)
                                .build(*engine);
    auto* attributeMaterial = filament::Material::Builder()
                                  .package(package.data(), package.size())
                                  .constant("colorFromAttribute", true)
                                  .build(*engine);
    if (uniformMaterial == nullptr || attributeMaterial == nullptr) {
        std::printf("specialization FAILED: a material did not build\n");
        return 1;
    }
    std::printf("specialization ok: two programs from one package\n");

    const float quad[] = {-1.0f, -1.0f, 1.0f, -1.0f, -1.0f, 1.0f, 1.0f, 1.0f};
    const std::uint16_t indices[] = {0, 1, 2, 1, 3, 2};
    auto* indexBuffer = filament::IndexBuffer::Builder()
                            .indexCount(6)
                            .bufferType(filament::IndexBuffer::IndexType::USHORT)
                            .build(*engine);
    indexBuffer->setBuffer(*engine, filament::IndexBuffer::BufferDescriptor(
                                        indices, sizeof indices, nullptr));

    // The one buffer every constant-paint drawable points its unread slot at. Sized for the
    // largest vertex count in the frame, which here is four.
    constexpr std::size_t kZeroVertices = 4;
    auto* zeroPaint = filament::BufferObject::Builder()
                          .size(kZeroVertices * sizeof(float) * 4)
                          .bindingType(filament::BufferObject::BindingType::VERTEX)
                          .build(*engine);
    std::vector<float> zeros(kZeroVertices * 4, 0.0f);
    zeroPaint->setBuffer(*engine, filament::BufferObject::BufferDescriptor(
                                      zeros.data(), zeros.size() * sizeof(float), nullptr));

    // Green, per vertex, which is what a data-driven drawable would carry.
    const float perVertex[] = {0.0f, 1.0f, 0.0f, 1.0f, 0.0f, 1.0f, 0.0f, 1.0f,
                               0.0f, 1.0f, 0.0f, 1.0f, 0.0f, 1.0f, 0.0f, 1.0f};
    auto* attributePaint = filament::BufferObject::Builder()
                               .size(sizeof perVertex)
                               .bindingType(filament::BufferObject::BindingType::VERTEX)
                               .build(*engine);
    attributePaint->setBuffer(*engine, filament::BufferObject::BufferDescriptor(
                                           perVertex, sizeof perVertex, nullptr));

    auto* positions = filament::BufferObject::Builder()
                          .size(sizeof quad)
                          .bindingType(filament::BufferObject::BindingType::VERTEX)
                          .build(*engine);
    positions->setBuffer(*engine,
                         filament::BufferObject::BufferDescriptor(quad, sizeof quad, nullptr));

    // Every vertex buffer declares the family's whole attribute set, whichever permutation the
    // drawable is. That is what makes one package enough.
    const auto makeVertices = [&](filament::BufferObject* paint) {
        auto* vertices = filament::VertexBuffer::Builder()
                             .vertexCount(4)
                             .bufferCount(2)
                             .enableBufferObjects()
                             .attribute(filament::VertexAttribute::POSITION, 0,
                                        filament::VertexBuffer::AttributeType::FLOAT2, 0, 8)
                             .attribute(filament::VertexAttribute::CUSTOM0, 1,
                                        filament::VertexBuffer::AttributeType::FLOAT4, 0, 16)
                             .build(*engine);
        vertices->setBufferObjectAt(*engine, 0, positions);
        vertices->setBufferObjectAt(*engine, 1, paint);
        return vertices;
    };

    const filament::math::mat4 passthrough{filament::math::float4{1.0, 0.0, 0.0, 0.0},
                                           filament::math::float4{0.0, -1.0, 0.0, 0.0},
                                           filament::math::float4{0.0, 0.0, 1.0, 0.0},
                                           filament::math::float4{0.0, 0.0, 0.0, 1.0}};

    struct Case {
        const char* name;
        filament::Material* material;
        filament::BufferObject* paint;
        const char* want;
    };
    // Red from the uniform, green from the attribute. The third case is the second constant-paint
    // drawable, sharing the *same* zero buffer object as the first.
    const Case cases[] = {
        {"uniform  ", uniformMaterial, zeroPaint, "red"},
        {"attribute", attributeMaterial, attributePaint, "green"},
        {"shared   ", uniformMaterial, zeroPaint, "red"},
    };

    bool allGood = true;
    for (const Case& probe : cases) {
        auto* scene = engine->createScene();
        view->setScene(scene);
        camera->setCustomProjection(passthrough, -1.0, 1.0);
        camera->setModelMatrix(filament::math::mat4f());

        auto* vertices = makeVertices(probe.paint);
        auto* instance = probe.material->createInstance();
        instance->setParameter("color", filament::math::float4{1.0f, 0.0f, 0.0f, 1.0f});

        utils::Entity entity = utils::EntityManager::get().create();
        filament::RenderableManager::Builder(1)
            .boundingBox({{-1, -1, -1}, {1, 1, 1}})
            .culling(false)
            .material(0, instance)
            .geometry(0, filament::RenderableManager::PrimitiveType::TRIANGLES, vertices,
                      indexBuffer, 0, 6)
            .build(*engine, entity);
        scene->addEntity(entity);

        std::vector<std::uint8_t> pixels(W * H * 4, 0);
        for (int warm = 0; warm < 2; warm++) {
            if (renderer->beginFrame(swapChain)) {
                renderer->render(view);
                renderer->endFrame();
            }
            engine->flushAndWait();
        }
        filament::backend::PixelBufferDescriptor pb(
            pixels.data(), pixels.size(), filament::backend::PixelDataFormat::RGBA,
            filament::backend::PixelDataType::UBYTE);
        if (renderer->beginFrame(swapChain)) {
            renderer->render(view);
            renderer->readPixels(0, 0, W, H, std::move(pb));
            renderer->endFrame();
        }
        engine->flushAndWait();

        const std::size_t centre = ((H / 2) * W + W / 2) * 4;
        const std::uint8_t r = pixels[centre], g = pixels[centre + 1], b = pixels[centre + 2];
        const char* seen = (r > 128 && g < 128)   ? "red"
                           : (g > 128 && r < 128) ? "green"
                           : (r < 40 && g < 40 && b < 40) ? "blank"
                                                          : "other";
        const bool good = std::string(seen) == probe.want;
        allGood = allGood && good;
        std::printf("%-10s want %-6s saw %-6s rgb(%3u,%3u,%3u)  %s\n", probe.name, probe.want, seen,
                    r, g, b, good ? "ok" : "<== FAILED");

        scene->remove(entity);
        engine->getRenderableManager().destroy(entity);
        utils::EntityManager::get().destroy(entity);
        engine->destroy(scene);
        engine->destroy(instance);
        engine->destroy(vertices);
        engine->flushAndWait();
    }

    std::printf("%s\n", allGood ? "VERDICT one package serves both permutations, and the "
                                  "constant-paint slot costs one shared buffer"
                                : "VERDICT the mechanism does not hold");
    return allGood ? 0 : 1;
}
