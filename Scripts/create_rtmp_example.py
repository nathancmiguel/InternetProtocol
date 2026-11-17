"""
Builds the RTMP example content of the InternetProtocol plugin:

    /InternetProtocol/Example/RTMP/M_RtmpScreen   unlit material, texture parameter "Video" -> emissive
    /InternetProtocol/Example/RTMP/RtmpExample    map: floor, lights, sky, player start, AIPRtmpExampleActor

Run it from the editor (Tools > Execute Python Script) or headless:

    UnrealEditor-Cmd.exe <Project>.uproject -run=pythonscript -script="<plugin>/Scripts/create_rtmp_example.py"

Needs the Python Editor Script Plugin. Existing assets with the same names are replaced.
"""

import unreal

FOLDER = "/InternetProtocol/Example/RTMP"
MATERIAL = FOLDER + "/M_RtmpScreen"
LEVEL = FOLDER + "/RtmpExample"

asset_lib = unreal.EditorAssetLibrary
asset_tools = unreal.AssetToolsHelpers.get_asset_tools()
mel = unreal.MaterialEditingLibrary


def log(message):
    unreal.log("create_rtmp_example: " + message)


def build_material():
    # An existing material is rebuilt in place. load_asset is used to detect it because the asset registry may
    # not have scanned plugin content yet (headless commandlet).
    material = unreal.load_asset(MATERIAL)
    if material:
        mel.delete_all_material_expressions(material)
    else:
        material = asset_tools.create_asset("M_RtmpScreen", FOLDER, unreal.Material, unreal.MaterialFactoryNew())
    if material is None:
        raise RuntimeError("could not create " + MATERIAL)
    material.set_editor_property("shading_model", unreal.MaterialShadingModel.MSM_UNLIT)
    sample = mel.create_material_expression(material, unreal.MaterialExpressionTextureSampleParameter2D, -400, 0)
    sample.set_editor_property("parameter_name", "Video")
    default_texture = unreal.load_asset("/Engine/EngineResources/Black.Black")
    if default_texture:
        sample.set_editor_property("texture", default_texture)
    mel.connect_material_property(sample, "RGB", unreal.MaterialProperty.MP_EMISSIVE_COLOR)
    mel.recompile_material(material)
    asset_lib.save_loaded_asset(material)
    log("material saved")
    return material


def spawn(actor_class, location, rotation=unreal.Rotator(0, 0, 0)):
    actors = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    return actors.spawn_actor_from_class(actor_class, location, rotation)


def build_level(material):
    levels = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
    actors = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    if unreal.load_asset(LEVEL):
        # Rebuild an existing map in place.
        if not levels.load_level(LEVEL):
            raise RuntimeError("could not open " + LEVEL)
        actors.destroy_actors(actors.get_all_level_actors())
    elif not levels.new_level(LEVEL):
        raise RuntimeError("could not create " + LEVEL)

    floor = spawn(unreal.StaticMeshActor, unreal.Vector(0, 0, 0))
    floor.set_actor_label("Floor")
    floor.static_mesh_component.set_static_mesh(unreal.load_asset("/Engine/BasicShapes/Plane.Plane"))
    floor.set_actor_scale3d(unreal.Vector(60, 60, 1))

    sun = spawn(unreal.DirectionalLight, unreal.Vector(0, 0, 800), unreal.Rotator(roll=0, pitch=-40, yaw=35))
    sun.set_actor_label("Sun")
    sun.light_component.set_editor_property("atmosphere_sun_light", True)
    # Movable lights: no lighting build needed for the example.
    sun.root_component.set_editor_property("mobility", unreal.ComponentMobility.MOVABLE)
    spawn(unreal.SkyAtmosphere, unreal.Vector(0, 0, 0)).set_actor_label("SkyAtmosphere")
    sky = spawn(unreal.SkyLight, unreal.Vector(0, 0, 600))
    sky.set_actor_label("SkyLight")
    sky.light_component.set_editor_property("real_time_capture", True)
    sky.root_component.set_editor_property("mobility", unreal.ComponentMobility.MOVABLE)

    # The screen is at the origin, facing -X: start in front of it.
    start = spawn(unreal.PlayerStart, unreal.Vector(-1000, 0, 250), unreal.Rotator(roll=0, pitch=0, yaw=0))
    start.set_actor_label("PlayerStart")

    example_class = unreal.load_class(None, "/Script/InternetProtocolExample.IPRtmpExampleActor")
    if example_class is None:
        raise RuntimeError("IPRtmpExampleActor not found: is the InternetProtocolExample module built and loaded?")
    example = spawn(example_class, unreal.Vector(0, 0, 0))
    example.set_actor_label("RtmpExample")
    example.set_editor_property("screen_material", material)

    # Simple flying pawn (WASD + mouse) regardless of the project's default game mode.
    world = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_editor_world()
    world.get_world_settings().set_editor_property("default_game_mode", unreal.GameModeBase)

    if not levels.save_current_level():
        raise RuntimeError("could not save " + LEVEL)
    log("level saved")


build_level(build_material())
log("done")
