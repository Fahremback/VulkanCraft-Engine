#include "Scene.hpp"
#include "Entity.hpp"
#include "Components.hpp"
#include "Serializer.hpp"
#include "AssetRegistry.hpp"
#include "RuntimePackage.hpp"
#include "ScriptRuntime.hpp"
#include "../src/engine/scripting/VisualScriptCanvas.hpp"
#include "../src/engine/scripting/ScriptGraphBridge.hpp"
#include "../src/engine/physics/PhysicsRuntime.hpp"
#include "../src/engine/physics/Ragdoll.hpp"
#include "../src/engine/animation/AnimationRuntime.hpp"
#include "../src/tools/BuildTools.hpp"
#include "../src/tools/ProjectConfig.hpp"
#include "../src/tools/BuildPipeline.hpp"
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>

using namespace Engine;
int main(){
 const auto root=std::filesystem::temp_directory_path()/("hermes-verify-empty-project-"+UUID().to_string());
 const auto fail=[&](const char*m){std::cerr<<m<<'\n';std::error_code ec;std::filesystem::remove_all(root,ec);return EXIT_FAILURE;};
 std::filesystem::create_directories(root);
 const auto generated=root/"EmptyProject";
 Tools::ProjectOptions projectOptions;projectOptions.name="EmptyProject";projectOptions.voxelPlugin=false;projectOptions.enginePath=std::filesystem::current_path()/"engine";
 const auto generatedResult=Tools::ProjectGenerator::generate(generated,projectOptions);
 if(!generatedResult||!std::filesystem::exists(generated/"Config/Plugins.ini"))return fail("project generation failed");
 {std::ifstream plugins(generated/"Config/Plugins.ini");std::string text((std::istreambuf_iterator<char>(plugins)),{});if(text.find("Voxel=true")!=std::string::npos||text.find("Voxel=ON")!=std::string::npos)return fail("empty project unexpectedly enables voxel");}

 Scene scene("Empty Project Scene");
 const Entity camera=scene.create_entity("Main Camera");scene.cameraComponents[camera.get_id()]={70,.1f,2000,true};scene.transformComponents[camera.get_id()].position={0,2,5};
 const Entity light=scene.create_entity("Sun");scene.lightComponents[light.get_id()]={{1,.95f,.85f},10000,1000,true};
 const Entity spot=scene.create_entity("Spot");scene.lightComponents[spot.get_id()]={{0.2f,.5f,1.0f},4000,18,true,LightType::Spot};
 const Entity meshEntity=scene.create_entity("Imported Triangle");scene.rigidbodyComponents[meshEntity.get_id()]={1,.5f,.1f,false,true};
 scene.weaponComponents[meshEntity.get_id()]={35.f,900.f,12,48,true,2.5f,true};
 scene.particleEmitterComponents[meshEntity.get_id()]={{0,.5f,0},{0,1,0},.3f,40,1,4,.4f,1.2f,.1f,0,{1,.5f,.2f,1},{1,1,1,0},{0,-5,0},.1f,0,.4f,50,true,true};
 scene.vehicleComponents[meshEntity.get_id()]={6000,0.6f,8000,0.4f,0.5f,2.8f,1.7f,1400,true,true};
 scene.ragdollComponents[meshEntity.get_id()]={true,0.7f,true,1.2f,{0,1,0}};
 scene.missionComponents[meshEntity.get_id()]={"Mission","Destroy 3 targets",3,"MissionComplete",true,false};
 scene.dialogueComponents[meshEntity.get_id()]={"D1","NPC","Hello!","Continue","D2",true,false};
 scene.audioComponents[meshEntity.get_id()]={"Audio/hello.ogg",0.8f,1.0f,true,true,true,false};
 scene.destructionComponents[meshEntity.get_id()]={{0.5f,0.5f,0.5f},8,25.0f,3.0f,8.0f,true,false};
 scene.navigationComponents[meshEntity.get_id()]={32,32,1.0f,3.0f,true};

 const auto source=root/"triangle.gltf";
 {std::ofstream f(source,std::ios::binary);f<<R"({"asset":{"version":"2.0"},"meshes":[{"primitives":[{"attributes":{"POSITION":0},"indices":1}]}],"accessors":[{"count":3},{"count":3}]})";}
 AssetRegistry registry;AssetPipeline pipeline(registry);pipeline.add_importer(std::make_unique<MeshImporter>());
 const auto imported=pipeline.import({source,root/"Cooked",1});
 if(!imported||imported.asset.type!=AssetType::Mesh||!std::filesystem::exists(imported.asset.cookedPath))return fail("mesh import/cook failed");
 scene.meshRendererComponents[meshEntity.get_id()]={imported.asset.id,{},true,true};

 ScriptGraphAsset script;const UUID eventId,constantId,setId;script.nodes={{eventId,ScriptNodeKind::Event,"OnStart"},{constantId,ScriptNodeKind::ConstantFloat,"","",1.0},{setId,ScriptNodeKind::SetVariable,"","initialized"}};script.links={{eventId,constantId},{constantId,setId}};
 const auto compiled=ScriptCompiler::compile(script);if(!compiled)return fail("visual script compile failed");ScriptVM vm;vm.load(compiled.program);vm.start_event("OnStart");if(vm.run(0)!=VMStatus::Completed||vm.float_variable("initialized")!=1.0)return fail("visual script execution failed");

 // §49 walkthrough, scripting pillar: author the player controller THROUGH the
 // editor's Script Canvas (bridge + VisualScriptCanvas), save it as the scene's
 // companion Initial.script, and execute it exactly like the shipped game
 // (OnStart then Tick with WASD -> moveX/moveY -> playerX/playerZ).
 {
  ScriptGraphAsset authored;
  const UUID startId, tickId;
  authored.nodes.push_back(TypedScriptNode{startId,ScriptNodeKind::Event,"OnStart"});
  authored.nodes.push_back(TypedScriptNode{UUID(),ScriptNodeKind::ConstantFloat,"","",0.0});
  authored.nodes.push_back(TypedScriptNode{UUID(),ScriptNodeKind::SetVariable,"","playerX"});
  authored.nodes.push_back(TypedScriptNode{UUID(),ScriptNodeKind::ConstantFloat,"","",0.0});
  authored.nodes.push_back(TypedScriptNode{UUID(),ScriptNodeKind::SetVariable,"","playerZ"});
  authored.nodes.push_back(TypedScriptNode{tickId,ScriptNodeKind::Event,"Tick"});
  authored.nodes.push_back(TypedScriptNode{UUID(),ScriptNodeKind::GetVariable,"","playerX"});
  authored.nodes.push_back(TypedScriptNode{UUID(),ScriptNodeKind::GetVariable,"","moveX"});
  authored.nodes.push_back(TypedScriptNode{UUID(),ScriptNodeKind::AddFloat});
  authored.nodes.push_back(TypedScriptNode{UUID(),ScriptNodeKind::SetVariable,"","playerX"});
  authored.nodes.push_back(TypedScriptNode{UUID(),ScriptNodeKind::GetVariable,"","playerZ"});
  authored.nodes.push_back(TypedScriptNode{UUID(),ScriptNodeKind::GetVariable,"","moveY"});
  authored.nodes.push_back(TypedScriptNode{UUID(),ScriptNodeKind::AddFloat});
  authored.nodes.push_back(TypedScriptNode{UUID(),ScriptNodeKind::SetVariable,"","playerZ"});
  const auto idOf=[](int index){return UUID(static_cast<uint64_t>(index+1), 0);};
  authored.links={{startId,idOf(1)},{idOf(1),idOf(2)},{idOf(2),idOf(3)},{idOf(3),idOf(4)},{tickId,idOf(6)},{idOf(6),idOf(7)},{idOf(7),idOf(8)},{idOf(8),idOf(9)},{idOf(9),idOf(10)},{idOf(10),idOf(11)},{idOf(11),idOf(12)},{idOf(12),idOf(13)}};

  // Editor path: asset -> typed-pin canvas -> back to asset (IDs preserved).
  VisualScriptCanvas canvas(to_visual_graph(authored));
  const ScriptGraphAsset roundtripped = from_visual_graph(canvas.graph());
  if (roundtripped.nodes.size() != authored.nodes.size()) return fail("canvas roundtrip node count");

  // Save the authored script into the generated project (the shipped game
  // prefers Content/Scenes/Initial.script over its built-in controller).
  const auto scriptPath = generated / "Content" / "Scenes" / "Initial.script";
  std::filesystem::create_directories(scriptPath.parent_path());
  if (!roundtripped.save(scriptPath)) return fail("authored script save failed");

  // Execute it like the game: load -> compile -> OnStart, then tick with input.
  ScriptGraphAsset loadedScript;
  if (!loadedScript.load(scriptPath)) return fail("authored script load failed");
  const auto compiledPlayer = ScriptCompiler::compile(loadedScript);
  if (!compiledPlayer) return fail("authored script compile failed");
  ScriptVM playerVM;
  playerVM.load(compiledPlayer.program);
  playerVM.start_event("OnStart");
  if (playerVM.run(0) != VMStatus::Completed) return fail("OnStart run failed");
  playerVM.set_variable("moveX", 1.0);
  playerVM.set_variable("moveY", 0.5);
  if (!playerVM.start_event("Tick") || playerVM.run(0) != VMStatus::Completed) return fail("Tick run failed");
  if (std::abs(playerVM.float_variable("playerX") - 1.0) > 1e-4f ||
      std::abs(playerVM.float_variable("playerZ") - 0.5) > 1e-4f) {
    return fail("gameplay script did not accumulate input");
  }
  // A second tick accumulates (the transform follows each frame).
  playerVM.set_variable("moveX", -0.25);
  playerVM.start_event("Tick");
  playerVM.run(0);
  if (std::abs(playerVM.float_variable("playerX") - 0.75) > 1e-4f) return fail("gameplay script tick accumulation");
  if (!std::filesystem::exists(scriptPath)) return fail("Initial.script missing from project");
 }

 Physics::PhysicsRuntime physics;Physics::BodyDesc body;body.position={0,4,0};body.collider.shape=Physics::BoxShape{{.5f,.5f,.5f}};const auto bodyId=physics.create_body(body);physics.step(1.0f/60.0f);const auto*simulated=physics.body(bodyId);if(!simulated||simulated->position.y>=4)return fail("physics did not execute");

 // Ragdoll: two capsule bodies joined by a distance constraint fall under
 // gravity, and the pose bridge blends their physics transforms into the
 // animated pose (the game drives the skinned mesh with this blend).
 {
  Physics::PhysicsRuntime ragWorld;
  std::vector<Physics::RagdollBoneDesc> ragBones;
  ragBones.push_back({"Root","",{0,0,0},{1,0,0,0},0.6f,0.12f,1.0f,{0,0,0}});
  ragBones.push_back({"Tip","Root",{1,0,0},{1,0,0,0},0.6f,0.12f,1.0f,{0,0,0}});
  Physics::Ragdoll ragdoll;
  if(!ragdoll.create(ragWorld,ragBones,{0,5,0}))return fail("ragdoll create failed");
  ragdoll.apply_impulse(ragWorld,"Tip",{2,1,0});
  for(int i=0;i<120;++i)ragWorld.step(1.0f/60.0f);
  const auto ragPose=ragdoll.pose(ragWorld);
  bool rootFell=false,tipMoved=false;
  for(const auto& bone:ragPose){if(bone.name=="Root"&&bone.position.y<4.2f)rootFell=true;if(bone.name=="Tip"&&glm::length(bone.position-glm::vec3(1,5,0))>0.5f)tipMoved=true;}
  if(!rootFell||!tipMoved)return fail("ragdoll did not simulate");

  // Pose bridge: a physics body at (0,2,0) on bone 0 blended at weight 1
  // must move that bone's local translation.
  SkeletonAsset ragSkeleton;ragSkeleton.bones.resize(2);
  ragSkeleton.bones[0].name="Root";ragSkeleton.bones[0].parentIndex=-1;
  ragSkeleton.bones[1].name="Tip";ragSkeleton.bones[1].parentIndex=0;
  Pose animated=AnimationSampler::bind_pose(ragSkeleton);
  std::vector<RagdollBody> ragBodies;
  ragBodies.push_back({0,glm::translate(glm::mat4(1.0f),glm::vec3(0,2,0)),1.0f});
  const Pose blended=RagdollPoseBridge::blend_physics_pose(ragSkeleton,animated,ragBodies,1.0f);
  if(std::abs(blended.local[0].translation.y-2.0f)>1e-4f)return fail("pose bridge blend failed");
  ragdoll.destroy(ragWorld);
 }

 // Ragdoll from a real skin (Fase 6): a three-bone skeleton becomes capsule
 // bodies via build_ragdoll_bones (world positions chained up the hierarchy),
 // simulates under gravity, and its pose reports the physics transforms.
 {
  SkeletonAsset skin;
  skin.name = "Hero";
  skin.bones.push_back({"Pelvis", -1, glm::translate(glm::mat4(1.0f), glm::vec3(0, 2, 0))});
  skin.bones.push_back({"Spine", 0,  glm::translate(glm::mat4(1.0f), glm::vec3(0, 0.5f, 0))});
  skin.bones.push_back({"Head", 1,   glm::translate(glm::mat4(1.0f), glm::vec3(0, 0.4f, 0))});
  const auto ragBones = Physics::build_ragdoll_bones(skin, 1.0f);
  if (ragBones.size() != 3) return fail("ragdoll from skeleton bone count mismatch");
  if (ragBones[0].name != "Pelvis" || ragBones[1].parent != "Pelvis" || ragBones[2].parent != "Spine") return fail("ragdoll parent chain mismatch");
  if (std::abs(ragBones[1].position.y - 2.5f) > 1e-4f || std::abs(ragBones[2].position.y - 2.9f) > 1e-4f) return fail("ragdoll world positions mismatch");
  Physics::PhysicsRuntime ragSkinWorld;
  Physics::Ragdoll skinRagdoll;
  if (!skinRagdoll.create(ragSkinWorld, ragBones, glm::vec3(0, 0, 0))) return fail("ragdoll from skeleton create failed");
  skinRagdoll.apply_impulse(ragSkinWorld, "Head", glm::vec3(1.5f, 0, 0));
  for (int i = 0; i < 120; ++i) ragSkinWorld.step(1.0f / 60.0f);
  bool fell = false, tipped = false;
  for (const auto& bone : skinRagdoll.pose(ragSkinWorld)) {
    if (bone.name == "Pelvis" && bone.position.y < 1.8f) fell = true;
    if (bone.name == "Head" && glm::length(bone.position - glm::vec3(0, 2.9f, 0)) > 0.5f) tipped = true;
  }
  if (!fell || !tipped) return fail("ragdoll from skeleton did not simulate");
  skinRagdoll.destroy(ragSkinWorld);
 }

 const auto scenePath=generated/"Content/Main.scene";std::filesystem::create_directories(scenePath.parent_path());if(!Serializer::serialize_scene(scene,scenePath))return fail("scene save failed");Scene loaded;if(!Serializer::deserialize_scene(loaded,scenePath))return fail("scene load failed");if(loaded.cameraComponents.size()!=1||loaded.lightComponents.size()!=2||loaded.meshRendererComponents.size()!=1||loaded.rigidbodyComponents.size()!=1||loaded.weaponComponents.size()!=1||loaded.particleEmitterComponents.size()!=1||loaded.vehicleComponents.size()!=1||loaded.ragdollComponents.size()!=1||loaded.missionComponents.size()!=1||loaded.dialogueComponents.size()!=1||loaded.audioComponents.size()!=1||loaded.destructionComponents.size()!=1||loaded.navigationComponents.size()!=1){std::cerr<<"roundtrip counts c="<<loaded.cameraComponents.size()<<" l="<<loaded.lightComponents.size()<<" m="<<loaded.meshRendererComponents.size()<<" r="<<loaded.rigidbodyComponents.size()<<" w="<<loaded.weaponComponents.size()<<" p="<<loaded.particleEmitterComponents.size()<<" v="<<loaded.vehicleComponents.size()<<" g="<<loaded.ragdollComponents.size()<<" mi="<<loaded.missionComponents.size()<<" d="<<loaded.dialogueComponents.size()<<" a="<<loaded.audioComponents.size()<<" x="<<loaded.destructionComponents.size()<<" n="<<loaded.navigationComponents.size()<<'\n';return fail("scene roundtrip failed");}
const auto& wpn=loaded.weaponComponents.at(meshEntity.get_id());if(wpn.damage!=35.f||wpn.magazineSize!=12||wpn.reserveAmmo!=48||wpn.automatic!=true||wpn.spreadDegrees!=2.5f)return fail("weapon component roundtrip failed");
const auto& pe=loaded.particleEmitterComponents.at(meshEntity.get_id());if(pe.rate!=40.f||pe.burstCount!=50||pe.collide!=true||pe.emitting!=true||pe.lifetimeMax!=1.2f)return fail("particle emitter roundtrip failed");
const auto& vc=loaded.vehicleComponents.at(meshEntity.get_id());if(vc.enginePower!=6000.f||vc.wheelBase!=2.8f||vc.trackWidth!=1.7f||vc.mass!=1400.f||vc.frontWheelDrive!=true)return fail("vehicle component roundtrip failed");
const auto& rg=loaded.ragdollComponents.at(meshEntity.get_id());if(rg.enabled!=true||rg.blendWeight!=0.7f||rg.fromSkeleton!=true||rg.massPerBone!=1.2f)return fail("ragdoll component roundtrip failed");
const auto& mc=loaded.missionComponents.at(meshEntity.get_id());if(mc.missionId!="Mission"||mc.objectiveTarget!=3||mc.completeEvent!="MissionComplete"||mc.autoStart!=true)return fail("mission component roundtrip failed");
const auto& dc=loaded.dialogueComponents.at(meshEntity.get_id());if(dc.character!="NPC"||dc.line!="Hello!"||dc.choiceText!="Continue"||dc.nextDialogueId!="D2"||dc.playOnStart!=true)return fail("dialogue component roundtrip failed");
const auto& ac=loaded.audioComponents.at(meshEntity.get_id());if(ac.clipPath!="Audio/hello.ogg"||ac.volume!=0.8f||ac.spatial!=true||ac.looping!=true||ac.playOnStart!=true)return fail("audio component roundtrip failed");
const auto& xc=loaded.destructionComponents.at(meshEntity.get_id());if(xc.chunkCount!=8||xc.chunkHealth!=25.0f||xc.damageRadius!=3.0f||xc.damageImpulse!=8.0f||xc.enabled!=true)return fail("destruction component roundtrip failed");
const auto& nc=loaded.navigationComponents.at(meshEntity.get_id());if(nc.gridWidth!=32||nc.gridHeight!=32||nc.cellSize!=1.0f||nc.agentSpeed!=3.0f||nc.enabled!=true)return fail("navigation component roundtrip failed");
if(loaded.lightComponents.at(spot.get_id()).type!=LightType::Spot)return fail("light type roundtrip failed");
 const auto registryDb=generated/"Intermediate/AssetRegistry.db";if(!registry.save(registryDb))return fail("asset registry save failed");
 const auto staged=generated/"StagedContent";const auto packaged=AssetPackager::package(registry,{imported.asset.id},staged);if(!packaged||!std::filesystem::exists(packaged.manifestPath))return fail("asset package failed");
 RuntimeAssetPackage runtimePackage;std::string mountError;if(!runtimePackage.mount(staged,&mountError)||!runtimePackage.find(imported.asset.id))return fail("runtime manifest load failed");

 const auto fakeGame=root/"EmptyGame.exe";{std::ofstream f(fakeGame,std::ios::binary);f<<"empty-game";}
 const auto shipping=root/"Shipping";const auto shippingResult=Tools::PackageBuilder::build(fakeGame,staged,shipping,{"Windows","Shipping"});if(!shippingResult||!std::filesystem::exists(shipping/"PackageManifest.txt"))return fail("shipping package failed");
 std::error_code ec;std::filesystem::remove_all(root,ec);

 // ProjectConfig + BuildPipeline: profiles, validation gating, and the full
 // build pipeline run end-to-end against a real temp project directory.
 {
     using namespace Engine::Tools;
     const std::filesystem::path projRoot = root / "PipelineProject";
     std::filesystem::create_directories(projRoot / "Assets");
     std::filesystem::create_directories(projRoot / "Scenes");
     {
         std::ofstream f(projRoot / "Scenes" / "Main.scene");
         f << "scene";
     }
     {
         std::ofstream f(projRoot / "Assets" / "hero.fbx");
         f << "fbx";
     }
     ProjectConfig config;
     config.name = "PipelineGame";
     config.projectDirectory = projRoot;
     #ifdef VULKANCRAFT_SOURCE_DIR
     config.enginePath = std::filesystem::path(VULKANCRAFT_SOURCE_DIR);
     #else
     config.enginePath = std::filesystem::current_path().parent_path().parent_path(); // engine/build/Release -> engine
     #endif
     config.initialScene = "Scenes/Main.scene";
     config.activeProfile = BuildProfile::Shipping;
     config.enabledPlugins = {"VoxelWorld", "Missions"};

     const auto report = config.validate();
     if (!report.valid()) return fail("pipeline project failed validation");
     if (!config.save_to_file() || !std::filesystem::exists(config.config_file_path()))
         return fail("project config save failed");
     ProjectConfig reloaded;
     reloaded.projectDirectory = projRoot;
     reloaded.enginePath = config.enginePath;
     if (!reloaded.load_from_file()) return fail("project config load failed");
     if (reloaded.name != "PipelineGame" || reloaded.activeProfile != BuildProfile::Shipping ||
         reloaded.targetPlatform != TargetPlatform::Windows ||
         reloaded.enabledPlugins.size() != 2 || !reloaded.is_plugin_enabled("Missions"))
         return fail("project config roundtrip mismatch");

     BuildPipeline pipeline(reloaded);
     const auto buildReport = pipeline.run();
     if (!buildReport.success) {
         const auto* last = buildReport.last_stage();
         const std::string msg = last ? std::string("build failed at ") + BuildPipeline::stage_name(last->stage)
                                      : std::string("build failed");
         return fail(msg.c_str());
     }
     if (buildReport.stages.size() != 9) return fail("build pipeline stage count mismatch");
     const auto distributable = projRoot / "Build" / "Shipping" / "Distributable";
     if (!std::filesystem::exists(distributable / "Binaries" / "PipelineGame.exe"))
         return fail("distributable executable missing");
     if (!std::filesystem::exists(distributable / "run_game.bat"))
         return fail("distributable launch script missing");

     // Invalid config must fail validation (gating works).
     ProjectConfig invalid = config;
     invalid.name = "";
     if (invalid.validate().valid()) return fail("empty-name config must not validate");

     // Plugin toggling.
     if (!config.set_plugin_enabled("Weapons", true) || !config.is_plugin_enabled("Weapons")) return fail("plugin enable failed");
     if (!config.set_plugin_enabled("Weapons", false) || config.is_plugin_enabled("Weapons")) return fail("plugin disable failed");
 }
 std::error_code ec2;std::filesystem::remove_all(root,ec2);
 std::cout<<"Empty project end-to-end flow passed without voxel dependencies.\n";return EXIT_SUCCESS;
 }
