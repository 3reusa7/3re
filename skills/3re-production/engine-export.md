# Engine export (v1.0.0)

* Delivery format: binary glTF 2.0 (`.glb`), Y-up, metres, standing on y = 0, front turned to +Z from the
  picture's registered front (`front: "picture"`). Validated by the Khronos glTF-Validator (0 errors required) and
  reloaded independently (tinygltf).
* Verified engine: **Godot 4** (headless import + instantiation, triangle count, materials, size). Unity and Unreal
  import glTF too (Unity through a glTF package such as glTFast; Unreal through its glTF importer), but 3re does not
  verify those imports: the chat must say "not verified" for them.
* Scale: the spec's metres along the height (characters) or the longest side (props); the report measures the error.

Sources: Khronos glTF-Validator https://github.com/KhronosGroup/glTF-Validator (Apache-2.0) ; Godot docs, importing 3D
scenes https://docs.godotengine.org/en/stable/tutorials/assets_pipeline/importing_3d_scenes/index.html ; Unity glTFast
https://docs.unity3d.com/Packages/com.unity.cloud.gltfast@latest ; Unreal glTF importer
https://dev.epicgames.com/documentation/en-us/unreal-engine/importing-gltf-files-into-unreal-engine .
