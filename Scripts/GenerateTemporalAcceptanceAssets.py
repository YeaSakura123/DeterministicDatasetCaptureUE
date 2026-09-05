"""Author a small, reproducible temporal acceptance scene in UE 5.7.

Run with UnrealEditor-Cmd -run=pythonscript -script=<this file> and
-EnablePlugins=PythonScriptPlugin,EditorScriptingUtilities,SequencerScripting.
Assets live in /Game/SRDatasetAcceptance; existing completed assets are preserved.
The scene deliberately uses ordinary meshes and Sequencer, without runtime fixture
motion, material clocks, physics caches, or hidden capture-specific actor updates.
"""
import unreal

ROOT = "/Game/SRDatasetAcceptance"
MAP = ROOT + "/TemporalLab"
ASSETS = unreal.AssetToolsHelpers.get_asset_tools()


def material(name, color=None):
    path = ROOT + "/" + name
    existing = unreal.load_asset(path)
    if existing:
        return existing
    mat = ASSETS.create_asset(name, ROOT, unreal.Material, unreal.MaterialFactoryNew())
    mat.set_editor_property("shading_model", unreal.MaterialShadingModel.MSM_UNLIT)
    lib = unreal.MaterialEditingLibrary
    if color is not None:
        expr = lib.create_material_expression(mat, unreal.MaterialExpressionConstant3Vector)
        expr.set_editor_property("constant", unreal.LinearColor(*color))
    else:
        uv = lib.create_material_expression(mat, unreal.MaterialExpressionTextureCoordinate)
        expr = lib.create_material_expression(mat, unreal.MaterialExpressionCustom)
        levels = "float3(.3,.04,.015),float3(.6,.08,.03)" if name == "M_MoverChecker" else "float3(.12,.12,.12),float3(.72,.72,.72)"
        expr.set_editor_property("code", "float v=fmod(floor(UV.x*16)+floor(UV.y*16),2); return lerp(" + levels + ",v);")
        expr.set_editor_property("output_type", unreal.CustomMaterialOutputType.CMOT_FLOAT3)
        custom_input = unreal.CustomInput()
        custom_input.set_editor_property("input_name", "UV")
        expr.set_editor_property("inputs", [custom_input])
        lib.connect_material_expressions(uv, "", expr, "UV")
    lib.connect_material_property(expr, "", unreal.MaterialProperty.MP_EMISSIVE_COLOR)
    lib.recompile_material(mat)
    unreal.EditorAssetLibrary.save_loaded_asset(mat)
    return mat


def mesh(label, pos, dimensions, mat):
    actor = unreal.EditorLevelLibrary.spawn_actor_from_class(unreal.StaticMeshActor, unreal.Vector(*pos))
    actor.set_actor_label(label)
    actor.set_editor_property("tags", [label])
    component = actor.static_mesh_component
    component.set_mobility(unreal.ComponentMobility.MOVABLE)
    component.set_static_mesh(unreal.load_asset("/Engine/BasicShapes/Cube"))
    component.set_material(0, mat)
    component.set_editor_property("cast_shadow", False)
    actor.set_actor_scale3d(unreal.Vector(*(v / 100 for v in dimensions)))
    return actor


def camera(label, y):
    actor = unreal.EditorLevelLibrary.spawn_actor_from_class(unreal.CameraActor, unreal.Vector(0, y, 200))
    actor.set_actor_label(label)
    actor.set_editor_property("tags", [label])
    actor.camera_component.set_field_of_view(90)
    actor.camera_component.set_editor_property("constrain_aspect_ratio", False)
    return actor


def transform(binding, pos0, pos1, count, scale=(1, 1, 1)):
    section = binding.add_track(unreal.MovieScene3DTransformTrack).add_section()
    section.set_range(0, count)
    channels = section.get_all_channels()
    for index, value in enumerate((*pos0, 0, 0, 0, *scale)):
        channels[index].set_default(float(value))
    for index, (v0, v1) in enumerate(zip(pos0, pos1)):
        channels[index].add_key(unreal.FrameNumber(0), float(v0), interpolation=unreal.MovieSceneKeyInterpolation.LINEAR)
        channels[index].add_key(unreal.FrameNumber(count - 1), float(v1), interpolation=unreal.MovieSceneKeyInterpolation.LINEAR)


def sequence(name, camera_a, camera_b, mover, camera_motion=False, object_motion=False, cut=False, camera_range=32):
    path = ROOT + "/" + name
    if unreal.load_asset(path):
        return
    count = 24 if cut else 64
    seq = ASSETS.create_asset(name, ROOT, unreal.LevelSequence, unreal.LevelSequenceFactoryNew())
    seq.set_display_rate(unreal.FrameRate(30, 1))
    seq.set_playback_start(0)
    seq.set_playback_end(count)
    a = seq.add_possessable(camera_a)
    b = seq.add_possessable(camera_b)
    m = seq.add_possessable(mover)
    transform(a, (0, -camera_range if camera_motion else 0, 200), (0, camera_range if camera_motion else 0, 200), count)
    transform(b, (0, 160, 200), (0, 160, 200), count)
    transform(m, (600, -120 if object_motion else -100, 200), (600, 120 if object_motion else -100, 200), count, (.1, 1.6, 2.2))
    track = seq.add_track(unreal.MovieSceneCameraCutTrack)
    for begin, end, binding in ([(0, 12, a), (12, count, b)] if cut else [(0, count, a)]):
        section = track.add_section()
        section.set_range(begin, end)
        section.set_camera_binding_id(seq.get_binding_id(binding))
    unreal.EditorAssetLibrary.save_loaded_asset(seq)


def main():
    checker = material("M_Checker")
    red = material("M_MoverChecker")
    white = material("M_White", (.9, .9, .9))
    if unreal.EditorAssetLibrary.does_asset_exist(MAP):
        unreal.EditorLevelLibrary.load_level(MAP)
        actors = {str(actor.get_actor_label()): actor for actor in unreal.EditorLevelLibrary.get_all_level_actors()}
        a, b, mover = (actors[name] for name in ("CameraA", "CameraB", "MovingPanel"))
        mover.static_mesh_component.set_material(0, red)
        unreal.EditorLevelLibrary.save_current_level()
    else:
        if not unreal.EditorLevelLibrary.new_level(MAP):
            raise RuntimeError("Could not create acceptance map")
        unreal.EditorLevelLibrary.get_editor_world().get_world_settings().set_editor_property("default_game_mode", unreal.GameModeBase)
        mesh("CheckerBackground", (1000, 0, 200), (10, 2400, 1400), checker)
        mover = mesh("MovingPanel", (600, -100, 200), (10, 160, 220), red)
        for index, width in enumerate((.6, 1.2, 2.4)):
            mesh(f"ThinLine{index}", (500, 150 + index * 45, 200), (1, width, 180), white)
        a, b = camera("CameraA", 0), camera("CameraB", 160)
        unreal.EditorLevelLibrary.save_current_level()
    for name, cm, om in (("Static", False, False), ("CameraOnly", True, False), ("ObjectOnly", False, True), ("Mixed", True, True)):
        sequence(name, a, b, mover, cm, om)
    sequence("CameraCut", a, b, mover, cut=True)
    sequence("CameraFast", a, b, mover, camera_motion=True, camera_range=96)
    unreal.log("SR temporal acceptance assets ready: " + MAP)


main()
