#include "SpecializedEditorsPanel.hpp"
#include "../WindowClamp.hpp"
#include <imgui.h>
#include <algorithm>

namespace Engine::Editor {
SpecializedEditorsPanel::SpecializedEditorsPanel(){
 const auto color=materialGraph_.add_constant("Base Color",glm::vec3(1.0f));
 const auto output=materialGraph_.add_output("BaseColor",Rendering::MaterialValueType::Vec3);
 (void)materialGraph_.connect(color,output,0);
 const auto hdr=renderGraph_.add_resource({"HDR Color",Rendering::RenderResourceKind::Image,0,1920,1080,1,true,false,Rendering::RenderResourceState::Undefined});
 const auto swap=renderGraph_.add_resource({"Swapchain",Rendering::RenderResourceKind::Image,0,1920,1080,1,false,true,Rendering::RenderResourceState::Present});
 const auto scenePass=renderGraph_.add_pass({"Scene",Rendering::RenderQueue::Graphics,{{hdr,Rendering::RenderAccess::Write,Rendering::RenderResourceState::ColorAttachment}},true});
 const auto presentPass=renderGraph_.add_pass({"Present",Rendering::RenderQueue::Graphics,{{hdr,Rendering::RenderAccess::Read,Rendering::RenderResourceState::ShaderRead},{swap,Rendering::RenderAccess::Write,Rendering::RenderResourceState::Present}},true});
 (void)renderGraph_.add_dependency(scenePass,presentPass);renderGraphViewer_.rebuild(renderGraph_);
}
void SpecializedEditorsPanel::draw_validation(const EditorDocumentModel& model){
 for(const auto& issue:model.validate()){
  const ImVec4 color=issue.severity==ValidationSeverity::Error?ImVec4(1,.3f,.3f,1):issue.severity==ValidationSeverity::Warning?ImVec4(1,.75f,.2f,1):ImVec4(.5f,.8f,1,1);
  ImGui::TextColored(color,"%s: %s",issue.field.c_str(),issue.message.c_str());
 }
}
void SpecializedEditorsPanel::draw(){
 if(!open)return;if(!ImGui::Begin("Specialized Editors",&open)){ImGui::End();return;}
 clamp_floating_window_on_screen();
 if(ImGui::BeginTabBar("##editors")){
  if(ImGui::BeginTabItem("Animation", nullptr, tab_flags("Animation"))){
   ImGui::Checkbox("Playing",&animation_.playing);ImGui::DragFloat("Preview Time",&animation_.previewTime,.01f,0,100);
   if(ImGui::Button("Add State")){const auto n=animation_.states.size();animation_.add_state({"State"+std::to_string(n+1),{},true,1});if(animation_.entryState.empty())animation_.entryState=animation_.states.back().id;}
   ImGui::SameLine();if(ImGui::Button("Add Transition")&&animation_.states.size()>1)animation_.transitions.push_back({animation_.states[animation_.states.size()-2].id,animation_.states.back().id,"speed > 0",.2f});
   for(auto&s:animation_.states){ImGui::PushID(&s);ImGui::Text("%s",s.id.c_str());ImGui::SameLine();ImGui::Checkbox("Loop",&s.loop);ImGui::SameLine();ImGui::DragFloat("Speed",&s.speed,.05f,.01f,5);ImGui::PopID();}draw_validation(animation_);
   // Panel -> scene integration: Apply writes the authored state machine
   // (states/transitions/entry) as a real AnimationComponent on the selected
   // entity; the play world samples clips and drives bone entity transforms.
   ImGui::Separator();
   const bool hasSelAn = scene_ && selected_.is_valid();
   ImGui::TextDisabled(hasSelAn ? "Apply to selected entity" : "Select an entity to apply");
   if(ImGui::Button("Apply to Selected") && hasSelAn){
    AnimationComponent ac; ac.entryState = animation_.entryState; ac.playing = animation_.playing;
    for(const auto& s : animation_.states) ac.states.push_back({s.id, s.clip, s.loop, s.speed});
    for(const auto& t : animation_.transitions) ac.transitions.push_back({t.from, t.to, t.condition, t.blendSeconds});
    scene_->animationComponents[selected_] = ac;
   }
   ImGui::SameLine();
   if(ImGui::Button("Remove from Selected") && hasSelAn) scene_->animationComponents.erase(selected_);
   ImGui::EndTabItem();
  }
  if(ImGui::BeginTabItem("Timeline", nullptr, tab_flags("Timeline"))){
   ImGui::TextColored(ImVec4(.32f,.55f,1,1), "ANIMATION TIMELINE");
   ImGui::Separator();
   ImGui::DragFloat("Duration",&timeline_.duration,.1f,.01f,10000);ImGui::SliderFloat("Playhead",&timeline_.playhead,0,std::max(timeline_.duration,.01f));ImGui::Checkbox("Loop",&timeline_.loop);
   ImGui::Separator();
   ImGui::TextDisabled("Procedural authoring");
   ImGui::SameLine();
   ImGui::Text("%s", (timeline_.tracks.empty() ? "Add a track to begin" : "Live scene document"));
   if(ImGui::Button("Add Animation Track"))timeline_.add_track({"Animation "+std::to_string(timeline_.tracks.size()+1),TimelineTrackType::Animation});
   ImGui::SameLine(); ImGui::TextDisabled("%zu tracks", timeline_.tracks.size());
   for(size_t i=0;i<timeline_.tracks.size();++i){auto&t=timeline_.tracks[i];ImGui::PushID(static_cast<int>(i));ImGui::Checkbox("Mute",&t.muted);ImGui::SameLine();ImGui::Text("%s (%zu keys)",t.name.c_str(),t.keys.size());ImGui::SameLine();if(ImGui::Button("Key"))timeline_.add_key(i,{timeline_.playhead,"value"});ImGui::SameLine(); ImGui::ProgressBar(timeline_.duration > 0 ? timeline_.playhead / timeline_.duration : 0.0f, ImVec2(-1, 0), ""); ImGui::PopID();}draw_validation(timeline_);
   // Panel -> scene integration: Apply writes the authored tracks as a real
   // TimelineComponent; the play world animates the entity transform from
   // Property tracks ("x y z"/"rx ry rz"/"sx sy sz" keys).
   ImGui::Separator();
   const bool hasSelTl = scene_ && selected_.is_valid();
   ImGui::TextDisabled(hasSelTl ? "Apply to selected entity" : "Select an entity to apply");
   if(ImGui::Button("Apply to Selected") && hasSelTl){
    TimelineComponent tc; tc.duration = std::max(timeline_.duration, 0.01f); tc.loop = timeline_.loop; tc.playhead = timeline_.playhead;
    for(const auto& t : timeline_.tracks){
     TimelineTrackDef td; td.name = t.name; td.type = static_cast<uint8_t>(t.type); td.muted = t.muted;
     for(const auto& k : t.keys) td.keys.push_back({k.time, k.value});
     tc.tracks.push_back(td);
    }
    scene_->timelineComponents[selected_] = tc;
   }
   ImGui::SameLine();
   if(ImGui::Button("Remove from Selected") && hasSelTl) scene_->timelineComponents.erase(selected_);
   ImGui::EndTabItem();
  }
  if(ImGui::BeginTabItem("Retarget", nullptr, tab_flags("Retarget"))){
   ImGui::TextColored(ImVec4(.32f,.55f,1,1), "SKELETON RETARGETING");
   ImGui::Separator();
   ImGui::Checkbox("Preserve Root Motion",&retarget_.preserveRootMotion);if(ImGui::Button("Auto-map Humanoid")){retarget_.map({"Root","Pelvis",1,{0,0,0}});retarget_.map({"Hand.L","Hand.L",1,{0,0,0}});} 
   ImGui::SameLine();
   if(ImGui::Button("Clear Mapping")){ retarget_.mapping.clear(); }
   ImGui::TextDisabled("Drag values below to author the live retarget document");
   ImGui::Text("Mappings: %zu", retarget_.mapping.size());
   if(ImGui::BeginTable("##RetargetMap", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)){ImGui::TableSetupColumn("Source");ImGui::TableSetupColumn("Target");ImGui::TableSetupColumn("Translation");ImGui::TableSetupColumn("Rotation");ImGui::TableHeadersRow();for(auto&m:retarget_.mapping){ImGui::TableNextRow();ImGui::TableNextColumn();ImGui::TextUnformatted(m.sourceBone.c_str());ImGui::TableNextColumn();ImGui::TextUnformatted(m.targetBone.c_str());ImGui::TableNextColumn();ImGui::PushID(&m);ImGui::DragFloat("##Scale",&m.translationScale,.01f,.01f,10);ImGui::PopID();ImGui::TableNextColumn();ImGui::Text("%.2f %.2f %.2f",m.rotationOffset.x,m.rotationOffset.y,m.rotationOffset.z);}ImGui::EndTable();}draw_validation(retarget_);
   // Panel -> scene integration: Apply writes the bone mapping as a real
   // RetargetComponent; the play world offsets target-bone transforms by the
   // authored scale/rotation while the source pose is played.
   ImGui::Separator();
   const bool hasSelRt = scene_ && selected_.is_valid();
   ImGui::TextDisabled(hasSelRt ? "Apply to selected entity" : "Select an entity to apply");
   if(ImGui::Button("Apply to Selected") && hasSelRt){
    RetargetComponent rc; rc.sourceSkeleton = retarget_.sourceSkeleton; rc.targetSkeleton = retarget_.targetSkeleton; rc.preserveRootMotion = retarget_.preserveRootMotion;
    for(const auto& m : retarget_.mapping) rc.mapping.push_back({m.sourceBone, m.targetBone, m.translationScale, m.rotationOffset});
    scene_->retargetComponents[selected_] = rc;
   }
   ImGui::SameLine();
   if(ImGui::Button("Remove from Selected") && hasSelRt) scene_->retargetComponents.erase(selected_);
   ImGui::EndTabItem();
  }   if(ImGui::BeginTabItem("IK", nullptr, tab_flags("IK"))){
   if(ImGui::Button("Add Two Bone Chain"))ik_.add_chain({"Chain"+std::to_string(ik_.chains.size()+1),"Upper","End",IKSolverKind::TwoBone,8,1,{0,1,0}});for(auto&c:ik_.chains){ImGui::PushID(&c);ImGui::Text("%s: %s -> %s",c.name.c_str(),c.rootBone.c_str(),c.endBone.c_str());ImGui::SliderFloat("Weight",&c.weight,0,1);ImGui::DragInt("Iterations",reinterpret_cast<int*>(&c.iterations),1,1,64);ImGui::PopID();}draw_validation(ik_);
   // Panel -> scene integration: Apply writes the first chain as a real
   // IKComponent. Bone names resolve to scene entities by name (the chain
   // name + "_Target" is the optional target entity). The play world runs
   // two-bone IK so the end entity reaches the target.
   ImGui::Separator();
   const bool hasSelIk = scene_ && selected_.is_valid();
   ImGui::TextDisabled(hasSelIk ? "Apply to selected entity" : "Select an entity to apply");
   if(ImGui::Button("Apply to Selected") && hasSelIk && !ik_.chains.empty()){
    const auto& c = ik_.chains.front();
    IKComponent ic; ic.poleVector = c.pole; ic.weight = c.weight; ic.iterations = static_cast<int>(c.iterations); ic.enabled = true;
    for(const auto& [id, ent] : scene_->get_entities()){
     if(ent.get_name() == c.rootBone) ic.rootEntity = id;
     else if(ent.get_name() == c.endBone) ic.endEntity = id;
     else if(ent.get_name() == c.name + "_Target") ic.targetEntity = id;
    }
    scene_->ikComponents[selected_] = ic;
   }
   ImGui::SameLine();
   if(ImGui::Button("Remove from Selected") && hasSelIk) scene_->ikComponents.erase(selected_);
   ImGui::EndTabItem();
  }
  if(ImGui::BeginTabItem("Ragdoll", nullptr, tab_flags("Ragdoll"))){
   ImGui::SliderFloat("Physics Blend",&ragdoll_.globalBlend,0,1);if(ImGui::Button("Add Body"))ragdoll_.add_body({"Bone"+std::to_string(ragdoll_.bodies.size()),RagdollShape::Capsule,{.2f,.5f,.2f},1,0});for(auto&b:ragdoll_.bodies){ImGui::PushID(&b);ImGui::Text("%s",b.bone.c_str());ImGui::DragFloat("Mass",&b.mass,.1f,.01f,100);ImGui::DragFloat3("Size",&b.size.x,.01f,.01f,10);ImGui::PopID();}draw_validation(ragdoll_);
   // Panel -> scene integration: the play world builds physics bodies per
   // bone from the entity's skin skeleton when fromSkeleton is checked.
   ImGui::Separator();
   const bool hasSelR = scene_ && selected_.is_valid();
   ImGui::TextDisabled(hasSelR ? "Apply to selected entity" : "Select an entity to apply");
   if(ImGui::Button("Apply to Selected") && hasSelR){
    RagdollComponent rg; rg.blendWeight = ragdoll_.globalBlend;
    scene_->ragdollComponents[selected_] = rg;
   }
   ImGui::SameLine();
   if(ImGui::Button("Remove from Selected") && hasSelR) scene_->ragdollComponents.erase(selected_);
   if(hasSelR){ ImGui::SameLine(); bool fromSkin = scene_->ragdollComponents[selected_].fromSkeleton; if(ImGui::Checkbox("From skeleton", &fromSkin)) scene_->ragdollComponents[selected_].fromSkeleton = fromSkin; }
   ImGui::EndTabItem();
  }   if(ImGui::BeginTabItem("Weapon", nullptr, tab_flags("Weapon"))){
   ImGui::DragFloat("Damage",&weapon_.damage,.5f,0,10000);ImGui::DragFloat("RPM",&weapon_.roundsPerMinute,5,1,5000);ImGui::DragFloat("Muzzle Velocity",&weapon_.muzzleVelocity,5,0,10000);ImGui::DragInt("Magazine",reinterpret_cast<int*>(&weapon_.magazine),1,1,1000);ImGui::Checkbox("Automatic",&weapon_.automatic);if(ImGui::Button("Add Recoil Key"))weapon_.add_recoil({weapon_.recoil.empty()?0:weapon_.recoil.back().time+.1f,{0,.2f}});ImGui::Text("Seconds per shot: %.3f",weapon_.seconds_per_shot());draw_validation(weapon_);
   // Panel → scene integration: apply the authored parameters as a real
   // WeaponComponent on the selected entity (the play world fires it).
   ImGui::Separator();
   const bool hasSel = scene_ && selected_.is_valid();
   ImGui::TextDisabled(hasSel ? "Apply to selected entity" : "Select an entity to apply");
   if(ImGui::Button("Apply to Selected") && hasSel){
    scene_->weaponComponents[selected_] = WeaponComponent{
     weapon_.damage, weapon_.roundsPerMinute, static_cast<uint32_t>(weapon_.magazine),
     static_cast<uint32_t>(weapon_.magazine * 3), weapon_.automatic, 1.5f, true };
   }
   ImGui::SameLine();
   if(ImGui::Button("Remove from Selected") && hasSel) scene_->weaponComponents.erase(selected_);
   ImGui::EndTabItem();
  }
  if(ImGui::BeginTabItem("Vehicle", nullptr, tab_flags("Vehicle"))){
   ImGui::DragFloat("Mass",&vehicle_.mass,10,1,100000);ImGui::DragFloat("Horsepower",&vehicle_.horsepower,5,0,10000);ImGui::DragFloat("Max RPM",&vehicle_.maxRpm,50,100,30000);if(ImGui::Button("Add Wheel"))vehicle_.add_wheel({"Wheel"+std::to_string(vehicle_.wheels.size()+1)});for(auto&w:vehicle_.wheels){ImGui::PushID(&w);ImGui::Text("%s",w.name.c_str());ImGui::DragFloat("Radius",&w.radius,.01f,.01f,5);ImGui::Checkbox("Steering",&w.steering);ImGui::SameLine();ImGui::Checkbox("Driven",&w.driven);ImGui::PopID();}draw_validation(vehicle_);
   // Panel -> scene integration (same contract as Weapon): the play world
   // builds the chassis + wheels from the authored VehicleComponent.
   ImGui::Separator();
   const bool hasSelV = scene_ && selected_.is_valid();
   ImGui::TextDisabled(hasSelV ? "Apply to selected entity" : "Select an entity to apply");
   if(ImGui::Button("Apply to Selected") && hasSelV){
    float wheelRadius = 0.36f; bool frontSteer = true;
    if(!vehicle_.wheels.empty()){ wheelRadius = vehicle_.wheels.front().radius; frontSteer = vehicle_.wheels.front().steering; }
    scene_->vehicleComponents[selected_] = VehicleComponent{
     vehicle_.horsepower * 1000.0f, 0.55f, 6000.0f, wheelRadius, 0.45f, 2.6f, 1.6f,
     std::max(vehicle_.mass, 1.0f), frontSteer, true };
   }
   ImGui::SameLine();
   if(ImGui::Button("Remove from Selected") && hasSelV) scene_->vehicleComponents.erase(selected_);
   ImGui::EndTabItem();
  }
  if(ImGui::BeginTabItem("Destruction", nullptr, tab_flags("Destruction"))){
   ImGui::DragInt("Seed",reinterpret_cast<int*>(&destruction_.seed));if(ImGui::Button("Add Fracture Level"))destruction_.add_level({"Level"+std::to_string(destruction_.levels.size()+1),32,10,1});for(auto&l:destruction_.levels){ImGui::PushID(&l);ImGui::Text("%s",l.name.c_str());ImGui::DragInt("Chunks",reinterpret_cast<int*>(&l.chunkCount),1,1,100000);ImGui::DragFloat("Threshold",&l.damageThreshold,.5f,0,10000);ImGui::PopID();}draw_validation(destruction_);
   // Panel -> scene integration: the play world builds a DestructibleRuntime
   // of chunkCount boxes; weapon hits apply radial damage.
   ImGui::Separator();
   const bool hasSelD = scene_ && selected_.is_valid();
   ImGui::TextDisabled(hasSelD ? "Apply to selected entity" : "Select an entity to apply");
   if(ImGui::Button("Apply to Selected") && hasSelD){
    uint32_t chunks = 6; float health = 25.0f;
    if(!destruction_.levels.empty()){ chunks = destruction_.levels.front().chunkCount; health = destruction_.levels.front().damageThreshold; }
    scene_->destructionComponents[selected_] = DestructionComponent{{0.5f,0.5f,0.5f}, std::max(chunks,1u), std::max(health,1.0f), 3.0f, 8.0f, true, false};
   }
   ImGui::SameLine();
   if(ImGui::Button("Remove from Selected") && hasSelD) scene_->destructionComponents.erase(selected_);
   ImGui::EndTabItem();
  }
  if(ImGui::BeginTabItem("Particles", nullptr, tab_flags("Particles"))){
   ImGui::DragFloat("Spawn Rate",&particle_.spawnRate,1,0,100000);ImGui::DragFloat("Lifetime",&particle_.lifetime,.1f,.01f,1000);ImGui::DragInt("Max Particles",reinterpret_cast<int*>(&particle_.maxParticles),10,1,10000000);ImGui::Checkbox("Looping",&particle_.looping);ImGui::SameLine();ImGui::Checkbox("Local Space",&particle_.localSpace);if(ImGui::Button("Add Velocity Module"))particle_.add_module({"Velocity",true,{{"speed",1}}});for(auto&m:particle_.modules){ImGui::PushID(&m);ImGui::Checkbox("Enabled",&m.enabled);ImGui::SameLine();ImGui::Text("%s",m.type.c_str());ImGui::PopID();}draw_validation(particle_);
   // Panel -> scene integration: the play world instantiates a
   // ParticleSimulation emitter from the authored ParticleEmitterComponent.
   ImGui::Separator();
   const bool hasSelP = scene_ && selected_.is_valid();
   ImGui::TextDisabled(hasSelP ? "Apply to selected entity" : "Select an entity to apply");
   if(ImGui::Button("Apply to Selected") && hasSelP){
    scene_->particleEmitterComponents[selected_] = ParticleEmitterComponent{
     {0,0,0},{0,1,0},0.4f, particle_.spawnRate, 1,3,
     std::max(particle_.lifetime*0.5f, 0.05f), std::max(particle_.lifetime, 0.1f),
     0.12f, 0, {1,1,1,1}, {1,1,1,0}, {0,-9.81f,0}, 0.05f, 0, 0.35f, 0, false, true };
   }
   ImGui::SameLine();
   if(ImGui::Button("Remove from Selected") && hasSelP) scene_->particleEmitterComponents.erase(selected_);
   ImGui::EndTabItem();
  }
  if(ImGui::BeginTabItem("Audio", nullptr, tab_flags("Audio"))){
   ImGui::DragFloat("Volume",&audio_.volume,.01f,0,4);ImGui::DragFloatRange2("Pitch",&audio_.minPitch,&audio_.maxPitch,.01f,.01f,4);ImGui::DragFloatRange2("Distance",&audio_.minDistance,&audio_.maxDistance,.1f,0,100000);ImGui::Checkbox("Spatial",&audio_.spatial);ImGui::SameLine();ImGui::Checkbox("Looping",&audio_.looping);if(ImGui::Button("Add Variation"))audio_.add_variation({{},1,1,1});ImGui::Text("Variations: %zu",audio_.variations.size());draw_validation(audio_);
   // Panel -> scene integration: the play world decodes the .ogg and plays it
   // through the Audio::Mixer (spatial against the camera listener).
   static char s_clipPath[256] = "";
   ImGui::Separator();
   const bool hasSelA = scene_ && selected_.is_valid();
   ImGui::TextDisabled(hasSelA ? "Apply to selected entity" : "Select an entity to apply");
   ImGui::InputText("Clip .ogg", s_clipPath, sizeof(s_clipPath));
   if(ImGui::Button("Apply to Selected") && hasSelA){
    scene_->audioComponents[selected_] = AudioComponent{ s_clipPath, audio_.volume, (audio_.minPitch+audio_.maxPitch)*0.5f, audio_.spatial, audio_.looping, true, false };
   }
   ImGui::SameLine();
   if(ImGui::Button("Remove from Selected") && hasSelA) scene_->audioComponents.erase(selected_);
   ImGui::EndTabItem();
  }
  if(ImGui::BeginTabItem("Navigation", nullptr, tab_flags("Navigation"))){
   ImGui::DragFloat("Cell Size",&navigation_.cellSize,.01f,.01f,10);ImGui::DragFloat("Agent Radius",&navigation_.agentRadius,.01f,.01f,10);ImGui::DragFloat("Agent Height",&navigation_.agentHeight,.01f,.01f,20);ImGui::SliderFloat("Max Slope",&navigation_.maxSlopeDegrees,0,90);ImGui::Checkbox("Debug Draw",&navigation_.debugDraw);if(ImGui::Button("Add Link"))navigation_.add_link({});ImGui::SameLine();if(ImGui::Button("Bake NavMesh"))navigation_.mark_saved();ImGui::Text("Links: %zu",navigation_.links.size());draw_validation(navigation_);
   // Panel -> scene integration: the play world bakes the NavigationGrid at
   // the entity and drives a NavigationAgent toward the camera entity.
   ImGui::Separator();
   const bool hasSelN = scene_ && selected_.is_valid();
   ImGui::TextDisabled(hasSelN ? "Apply to selected entity" : "Select an entity to apply");
   if(ImGui::Button("Apply to Selected") && hasSelN){
    scene_->navigationComponents[selected_] = NavigationComponent{ 32, 32, std::max(navigation_.cellSize, 0.1f), 3.0f, true };
   }
   ImGui::SameLine();
   if(ImGui::Button("Remove from Selected") && hasSelN) scene_->navigationComponents.erase(selected_);
   ImGui::EndTabItem();
  }
  if(ImGui::BeginTabItem("Mission", nullptr, tab_flags("Mission"))){
   if(ImGui::Button("Add Entry"))mission_.add_node({static_cast<uint64_t>(mission_.nodes.size()+1),MissionNodeKind::Entry,"Entry","",{0,0}});ImGui::SameLine();if(ImGui::Button("Add Objective"))mission_.add_node({static_cast<uint64_t>(mission_.nodes.size()+1),MissionNodeKind::Objective,"Objective","",{100,0}});for(auto&n:mission_.nodes)ImGui::BulletText("%llu %s",static_cast<unsigned long long>(n.id),n.title.c_str());draw_validation(mission_);
   // Panel -> scene integration: the play world runs the mission graph
   // (Start -> SetObjective -> WaitForEvent(completeEvent) -> Complete).
   ImGui::Separator();
   const bool hasSelM = scene_ && selected_.is_valid();
   ImGui::TextDisabled(hasSelM ? "Apply to selected entity" : "Select an entity to apply");
   if(ImGui::Button("Apply to Selected") && hasSelM){
    std::string objective = "Complete the objective"; uint32_t target = 1;
    for(const auto& n : mission_.nodes) if(n.kind == MissionNodeKind::Objective){ objective = n.title; target = static_cast<uint32_t>(n.id); break; }
    scene_->missionComponents[selected_] = MissionComponent{ "Mission", objective, std::max(target,1u), "MissionComplete", true, false };
   }
   ImGui::SameLine();
   if(ImGui::Button("Remove from Selected") && hasSelM) scene_->missionComponents.erase(selected_);
   ImGui::EndTabItem();
  }
  if(ImGui::BeginTabItem("Dialogue", nullptr, tab_flags("Dialogue"))){
   if(ImGui::Button("Add Dialogue Node")){uint64_t id=dialogue_.nodes.size()+1;dialogue_.add_node({id,"Speaker","Dialogue text",{0,0},{}});if(!dialogue_.entry)dialogue_.entry=id;}for(auto&n:dialogue_.nodes)ImGui::BulletText("%llu [%s] %s",static_cast<unsigned long long>(n.id),n.speaker.c_str(),n.text.c_str());draw_validation(dialogue_);
   // Panel -> scene integration: the play world registers the one-node graph
   // and plays it on start when playOnStart is set.
   ImGui::Separator();
   const bool hasSelT = scene_ && selected_.is_valid();
   ImGui::TextDisabled(hasSelT ? "Apply to selected entity" : "Select an entity to apply");
   if(ImGui::Button("Apply to Selected") && hasSelT){
    std::string speaker = "NPC"; std::string line = "Hello!";
    if(!dialogue_.nodes.empty()){ speaker = dialogue_.nodes.front().speaker; line = dialogue_.nodes.front().text; }
    scene_->dialogueComponents[selected_] = DialogueComponent{ "Dialogue", speaker, line, "Continue", "", true, false };
   }
   ImGui::SameLine();
   if(ImGui::Button("Remove from Selected") && hasSelT) scene_->dialogueComponents.erase(selected_);
   ImGui::EndTabItem();
  }
  if(ImGui::BeginTabItem("Physics", nullptr, tab_flags("Physics"))){
   ImGui::DragFloat("Mass",&physics_.mass,.1f,.01f,100000);ImGui::DragFloat3("Center of Mass",&physics_.centerOfMass.x,.01f);ImGui::Checkbox("Kinematic",&physics_.kinematic);ImGui::SameLine();ImGui::Checkbox("CCD",&physics_.continuousCollision);if(ImGui::Button("Add Box Collider"))physics_.add_collider({ColliderKind::Box});ImGui::Text("Colliders: %zu Constraints: %zu",physics_.colliders.size(),physics_.constraints.size());draw_validation(physics_);
   // Panel -> scene integration: apply/remove the authored body parameters
   // as a RigidbodyComponent on the selected entity (the play world simulates
   // it with the editor physics).
   ImGui::Separator();
   const bool hasSelPh = scene_ && selected_.is_valid();
   ImGui::TextDisabled(hasSelPh ? "Apply to selected entity" : "Select an entity to apply");
   if(ImGui::Button("Apply to Selected") && hasSelPh){
    scene_->rigidbodyComponents[selected_] = RigidbodyComponent{
     std::max(physics_.mass, 0.01f), 0.5f, 0.1f, physics_.kinematic, true };
   }
   ImGui::SameLine();
   if(ImGui::Button("Remove from Selected") && hasSelPh) scene_->rigidbodyComponents.erase(selected_);
   ImGui::EndTabItem();
  }
  if(ImGui::BeginTabItem("Material", nullptr, tab_flags("Material"))){
   if(ImGui::Button("Add scalar"))(void)materialGraph_.add_constant("Scalar",1.0f);
   ImGui::SameLine(); if(ImGui::Button("Add texture")){
    const auto id = materialGraph_.add_texture_sample("Texture Sample");
    auto* node = materialGraph_.find_node(id);
    if(node && !textureAssets_.empty()) node->value = textureAssets_.front().second.to_string();
    // Auto-connect to a free BaseColor output (RGBA→RGB swizzle at the sink);
    // replace the demo constant so the texture shows immediately.
    for(const auto& candidate : materialGraph_.nodes()){
     if(candidate.kind != Rendering::MaterialNodeKind::Output || candidate.parameter != "BaseColor") continue;
     auto* outNode = materialGraph_.find_node(candidate.id);
     if(!outNode || outNode->inputs.empty()) continue;
     const bool replace = outNode->inputs[0].source != Rendering::InvalidMaterialNode &&
                          materialGraph_.find_node(outNode->inputs[0].source) &&
                          materialGraph_.find_node(outNode->inputs[0].source)->kind == Rendering::MaterialNodeKind::Constant;
     if(outNode->inputs[0].source == Rendering::InvalidMaterialNode || replace){
      (void)materialGraph_.connect(id, outNode->id, 0);
      break;
     }
    }
    materialEditor_.compile_preview();
   }
   ImGui::SameLine(); if(ImGui::Button("Compile"))(void)materialEditor_.compile_preview();
   ImGui::SameLine(); ImGui::Checkbox("Preview on selected", &previewOnSelected);
   if(previewOnSelected)ImGui::TextColored({.5f,.8f,1,1},"%s","Mesh material: graph rendered on selected entity in the viewport");
   for(const auto& n:materialEditor_.nodes()){
    ImGui::BulletText("%u %s",n.id,n.title.c_str());
    if(n.kind != Rendering::MaterialNodeKind::TextureSample) continue;
    auto* node = materialGraph_.find_node(n.id);
    if(!node || textureAssets_.empty()) continue;
    const std::string* curStr = std::get_if<std::string>(&node->value);
    const std::string current = curStr ? *curStr : std::string();
    int currentIndex = -1;
    for(size_t i = 0; i < textureAssets_.size(); ++i)
     if(textureAssets_[i].second.to_string() == current){ currentIndex = static_cast<int>(i); break; }
    ImGui::SameLine();
    const std::string preview = currentIndex >= 0 ? textureAssets_[currentIndex].first : "(select texture)";
    if(ImGui::BeginCombo(("##tex" + std::to_string(n.id)).c_str(), preview.c_str())){
     for(size_t i = 0; i < textureAssets_.size(); ++i){
      const bool selected = static_cast<int>(i) == currentIndex;
      if(ImGui::Selectable(textureAssets_[i].first.c_str(), selected)) node->value = textureAssets_[i].second.to_string();
      if(selected) ImGui::SetItemDefaultFocus();
     }
     ImGui::EndCombo();
    }
   }
   for(const auto& e:materialEditor_.last_compile().errors)ImGui::TextColored({1,.3f,.3f,1},"%s",e.message.c_str());
   // Panel -> scene integration (same contract as Weapon/Vehicle/Particle):
   // Apply writes the authored graph's Base Color constant (and optional
   // Roughness/Metallic scalars) into the selected entity's MaterialComponent
   // so the graph also drives the base renderer, not just the viewport preview.
   ImGui::Separator();
   const bool hasSelMat = scene_ && selected_.is_valid();
   ImGui::TextDisabled(hasSelMat ? "Apply to selected entity" : "Select an entity to apply");
   if(ImGui::Button("Apply to Selected") && hasSelMat){
    glm::vec3 albedo{1.0f}; float roughness = 0.5f; float metallic = 0.0f;
    const auto lowerLabel = [](const std::string& s) {
     std::string out = s;
     std::transform(out.begin(), out.end(), out.begin(),
                    [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
     return out;
    };
    for(const auto& n : materialGraph_.nodes()){
     if(n.kind != Rendering::MaterialNodeKind::Constant) continue;
     const std::string label = lowerLabel(n.label);
     if(const auto* v3 = std::get_if<glm::vec3>(&n.value)){
      if(label.find("color") != std::string::npos || label.find("base") != std::string::npos) albedo = *v3;
     } else if(const float* f1 = std::get_if<float>(&n.value)){
      if(label.find("rough") != std::string::npos) roughness = *f1;
      else if(label.find("metal") != std::string::npos) metallic = *f1;
     }
    }
    scene_->materialComponents[selected_] = MaterialComponent{albedo, roughness, metallic, glm::vec3(0.0f), 0.0f};
   }
   ImGui::SameLine();
   if(ImGui::Button("Remove from Selected") && hasSelMat) scene_->materialComponents.erase(selected_);
   ImGui::EndTabItem();
  }
  if(ImGui::BeginTabItem("Render Graph", nullptr, tab_flags("Render Graph"))){
   if(ImGui::Button("Rebuild"))renderGraphViewer_.rebuild(renderGraph_);for(const auto& p:renderGraphViewer_.passes())ImGui::BulletText("%u %s",p.compiledIndex,p.name.c_str());
   ImGui::Text("Resources: %d | Barriers: %d",(int)renderGraphViewer_.resources().size(),(int)renderGraphViewer_.compilation().barriers.size());for(const auto& e:renderGraphViewer_.compilation().errors)ImGui::TextColored({1,.3f,.3f,1},"%s",e.c_str());ImGui::EndTabItem();
  }
  ImGui::EndTabBar();
 }
 ImGui::End();
}
} // namespace Engine::Editor
