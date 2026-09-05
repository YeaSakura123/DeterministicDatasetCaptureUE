"""Create four original UE 5.7 dataset maps, textures, lights and camera tracks.

Run with -run=pythonscript -script=<this file> and the PythonScriptPlugin,
EditorScriptingUtilities and SequencerScripting plugins enabled. Uses only
engine basic meshes and original generated texture pixels; no project content
or downloaded assets are copied. Assets are authored under /Game/SRFormalV1.
"""
import hashlib
import json
import math
import struct
from pathlib import Path

import unreal

ROOT = "/Game/SRFormalV1"
TOOLS = unreal.AssetToolsHelpers.get_asset_tools()
LIB = unreal.MaterialEditingLibrary
SOURCE = Path(unreal.Paths.project_saved_dir()) / "SRFormalAssets"
SOURCE.mkdir(parents=True, exist_ok=True)
ACTORS = {}


def connect(source, output, destination, input_name):
    if not LIB.connect_material_expressions(source, output, destination, input_name):
        raise RuntimeError(f"Material connection failed: {source.get_name()}:{output} -> {destination.get_name()}:{input_name}")


def texture(index):
    name = f"T_Surface{index}"
    old = unreal.load_asset(ROOT + "/" + name)
    if old:
        return old
    size = 512
    pixels = bytearray()
    palettes = [((185, 154, 96), (37, 66, 84)), ((154, 183, 161), (52, 87, 62)), ((150, 155, 180), (65, 58, 94)), ((218, 110, 65), (38, 137, 168))]
    colors = palettes[index]
    for y in range(size):
        for x in range(size):
            if index == 0:
                pattern = ((x // 8) + (y // 32)) % 2
            elif index == 1:
                pattern = int((x + (y // 32 % 2) * 24) % 64 < 3 or y % 32 < 3)
            elif index == 2:
                pattern = int(x % 16 < 2 or y % 64 < 2)
            else:
                pattern = int((x + 2 * y) % 23 < 5)
            color = colors[pattern]
            variation = ((x * 17 + y * 31) % 11) - 5
            pixels.extend(max(0, min(255, v + variation)) for v in reversed(color))
    path = SOURCE / (name + ".tga")
    path.write_bytes(struct.pack("<BBBHHBHHHHBB", 0, 0, 2, 0, 0, 0, 0, 0, size, size, 24, 32) + pixels)
    task = unreal.AssetImportTask()
    # A specific factory keeps unattended commandlets out of Interchange's
    # Content Browser notification path, which requires an initialized Slate UI.
    task.set_editor_property("factory", unreal.TextureFactory())
    for key, value in dict(filename=str(path), destination_path=ROOT, destination_name=name, automated=True, save=True).items():
        task.set_editor_property(key, value)
    TOOLS.import_asset_tasks([task])
    result = unreal.load_asset(ROOT + "/" + name)
    if not result:
        raise RuntimeError("Texture import failed: " + name)
    return result


def scalar(mat, value, prop):
    expr = LIB.create_material_expression(mat, unreal.MaterialExpressionConstant)
    expr.set_editor_property("r", value)
    if not LIB.connect_material_property(expr, "", prop):
        raise RuntimeError(f"Could not connect material scalar to {prop}")


def material(name, tex=None, color=(.5, .5, .5), roughness=.6, metallic=0, glass=False, wpo=False):
    old = unreal.load_asset(ROOT + "/" + name)
    if old and not wpo:
        return old
    mat = old or TOOLS.create_asset(name, ROOT, unreal.Material, unreal.MaterialFactoryNew())
    if old:
        LIB.delete_all_material_expressions(mat)
    mat.set_editor_property("shading_model", unreal.MaterialShadingModel.MSM_DEFAULT_LIT)
    if glass:
        mat.set_editor_property("blend_mode", unreal.BlendMode.BLEND_TRANSLUCENT)
        mat.set_editor_property("two_sided", True)
        # AfterDOF participates in the plugin's explicit transparency coverage.
        mat.set_editor_property("translucency_pass", unreal.MaterialTranslucencyPass.MTP_AFTER_DOF)
        scalar(mat, .35, unreal.MaterialProperty.MP_OPACITY)
    if tex:
        expr = LIB.create_material_expression(mat, unreal.MaterialExpressionTextureSample)
        expr.set_editor_property("texture", tex)
        uv = LIB.create_material_expression(mat, unreal.MaterialExpressionTextureCoordinate)
        uv.set_editor_property("u_tiling", 6.0)
        uv.set_editor_property("v_tiling", 6.0)
        connect(uv, "", expr, "UVs")
        output = "RGB"
    else:
        expr = LIB.create_material_expression(mat, unreal.MaterialExpressionConstant3Vector)
        expr.set_editor_property("constant", unreal.LinearColor(*color))
        output = ""
    if not LIB.connect_material_property(expr, output, unreal.MaterialProperty.MP_BASE_COLOR):
        raise RuntimeError("Could not connect material base color")
    scalar(mat, roughness, unreal.MaterialProperty.MP_ROUGHNESS)
    scalar(mat, metallic, unreal.MaterialProperty.MP_METALLIC)
    if wpo:
        mat.set_editor_property("always_evaluate_world_position_offset", True)
        clock = LIB.create_material_expression(mat, unreal.MaterialExpressionTime)
        wave = LIB.create_material_expression(mat, unreal.MaterialExpressionSine)
        wave.set_editor_property("period", 1.0)
        direction = LIB.create_material_expression(mat, unreal.MaterialExpressionConstant3Vector)
        direction.set_editor_property("constant", unreal.LinearColor(0, 40, 0))
        offset = LIB.create_material_expression(mat, unreal.MaterialExpressionMultiply)
        connect(clock, "", wave, "")
        connect(wave, "", offset, "A")
        connect(direction, "", offset, "B")
        if not LIB.connect_material_property(offset, "", unreal.MaterialProperty.MP_WORLD_POSITION_OFFSET):
            raise RuntimeError("Could not connect world position offset")
        # UE compiles Time to View.GameTime and View.PrevFrameGameTime in
        # current/previous velocity branches. The plugin supplies their actual
        # logical interval, including reverse FG endpoint time.
    LIB.recompile_material(mat)
    unreal.EditorAssetLibrary.save_loaded_asset(mat)
    return mat


def spawn(kind, label, location, rotation=(0, 0, 0)):
    actor = ACTORS.get(label)
    if actor is None:
        actor = unreal.EditorLevelLibrary.spawn_actor_from_class(kind, unreal.Vector(*location), unreal.Rotator(*rotation))
        actor.set_actor_label(label)
        actor.set_editor_property("tags", [label])
        ACTORS[label] = actor
    actor.set_actor_location(unreal.Vector(*location), False, False)
    actor.set_actor_rotation(unreal.Rotator(*rotation), False)
    return actor


def mesh(label, location, scale, mat, shape="Cube", yaw=0):
    actor = spawn(unreal.StaticMeshActor, label, location, (0, yaw, 0))
    component = actor.static_mesh_component
    component.set_mobility(unreal.ComponentMobility.MOVABLE)
    component.set_static_mesh(unreal.load_asset("/Engine/BasicShapes/" + shape))
    component.set_material(0, mat)
    actor.set_actor_scale3d(unreal.Vector(*scale))
    return actor


def add_transform(seq, actor, keyframes):
    binding = seq.add_possessable(actor)
    section = binding.add_track(unreal.MovieScene3DTransformTrack).add_section()
    section.set_range(0, 600)
    channels = section.get_all_channels()
    scale = actor.get_actor_scale3d()
    for index, value in enumerate((0, 0, 0, 0, 0, 0, scale.x, scale.y, scale.z)):
        channels[index].set_default(value)
    for frame, position, rotation in keyframes:
        # Sequencer's rotation channels are Roll, Pitch, Yaw.
        for channel, value in zip(channels[:6], (*position, rotation[2], rotation[0], rotation[1])):
            channel.add_key(unreal.FrameNumber(frame), float(value), interpolation=unreal.MovieSceneKeyInterpolation.LINEAR)
    return binding


def sequence(map_name, name, camera, mover, phase):
    asset_name = map_name + "_" + name
    if unreal.load_asset(ROOT + "/" + asset_name):
        return
    seq = TOOLS.create_asset(asset_name, ROOT, unreal.LevelSequence, unreal.LevelSequenceFactoryNew())
    seq.set_display_rate(unreal.FrameRate(30, 1))
    seq.set_playback_start(0)
    seq.set_playback_end(600)
    camera_keys, mover_keys = [], []
    for frame in list(range(0, 600, 20)) + [599]:
        t = frame / 599
        camera_keys.append((frame, (-80 + 240 * t, 110 * math.sin(2 * math.pi * t + phase), 175 + 15 * math.sin(math.pi * t + phase)), (-3, 5 * math.sin(math.pi * t + phase), 0)))
        mover_keys.append((frame, (650 + 80 * math.cos(2 * math.pi * t), 240 * math.sin(2 * math.pi * t + phase), 115), (0, 30 * math.sin(2 * math.pi * t), 0)))
    binding = add_transform(seq, camera, camera_keys)
    add_transform(seq, mover, mover_keys)
    section = seq.add_track(unreal.MovieSceneCameraCutTrack).add_section()
    section.set_range(0, 600)
    section.set_camera_binding_id(seq.get_binding_id(binding))
    unreal.EditorAssetLibrary.save_loaded_asset(seq)


def scene(index, name, surface, accent, glass, wave):
    global ACTORS
    path = ROOT + "/" + name
    if unreal.EditorAssetLibrary.does_asset_exist(path):
        unreal.EditorLevelLibrary.load_level(path)
    elif not unreal.EditorLevelLibrary.new_level(path):
        raise RuntimeError("Map creation failed: " + path)
    ACTORS = {str(a.get_actor_label()): a for a in unreal.EditorLevelLibrary.get_all_level_actors()}
    unreal.EditorLevelLibrary.get_editor_world().get_world_settings().set_editor_property("default_game_mode", unreal.GameModeBase)
    mesh("Floor", (700, 0, -15), (25, 24, .3), surface)
    mesh("BackWall", (1650, 0, 300), (.3, 24, 6), surface)
    if index == 0:  # A gallery of framed reliefs and thin vertical occluders.
        for i in range(9):
            mesh(f"GalleryColumn{i}", (700 + (i % 3) * 300, -640 + (i // 3) * 550, 180), (.2 + i * .025, .2, 3.6), accent)
        for i in range(5):
            mesh(f"Relief{i}", (1580, -720 + 360 * i, 240), (.2, 2.5, 2.5), accent, yaw=i * 5)
    elif index == 1:  # Open courtyard with round pillars and stepped platforms.
        for i in range(10):
            angle = i * math.tau / 10
            mesh(f"Pillar{i}", (1050 + 400 * math.cos(angle), 700 * math.sin(angle), 190), (.65, .65, 3.8), surface, "Cylinder")
        for i in range(4):
            mesh(f"Step{i}", (1300 + i * 70, 0, 20 + i * 25), (1.5, 6 - i, .4 + i * .5), accent)
    elif index == 2:  # Workshop racks with different shelf depths and machinery.
        for side in (-1, 1):
            for rack in range(3):
                for shelf in range(4):
                    mesh(f"Shelf{side}_{rack}_{shelf}", (600 + rack * 330, side * 640, 55 + shelf * 95), (2.8, 2.2, .12), accent)
                mesh(f"Machine{side}_{rack}", (620 + rack * 330, side * 600, 130), (1.3, 1.4, 2), surface)
    else:  # Unseen diagonal arrangement, cones, and slanted screens.
        for i in range(7):
            mesh(f"Cone{i}", (500 + i * 140, -650 + (i % 3) * 500, 140), (1.2, 1.2, 2.8), accent, "Cone")
        for i in range(4):
            mesh(f"Diagonal{i}", (900 + i * 160, -400 + i * 270, 220), (.18, 3, 4.4), surface, yaw=25 + i * 7)
    for i in range(3):
        mesh(f"GlossSphere{i}", (900 + i * 210, -320 + i * 320, 110), (1.6, 1.6, 1.6), accent, "Sphere")
    mesh("GlassScreen", (480, 360, 175), (.03, 2.4, 2.8), glass)
    mesh("WPOPanel", (540, -250, 170), (.08, 1.2, 2), wave)
    mover = mesh("MovingObject", (730, 0, 115), (1, 1.3, 2.3), accent)
    camera = spawn(unreal.CameraActor, "DatasetCamera", (-80, 0, 175), (-3, 0, 0))
    camera.camera_component.set_field_of_view(90)
    camera.camera_component.set_editor_property("constrain_aspect_ratio", False)
    light = spawn(unreal.DirectionalLight, "KeyLight", (0, 0, 1000), (-35 - index * 5, -25 + index * 20, 0))
    light.light_component.set_mobility(unreal.ComponentMobility.MOVABLE)
    light.light_component.set_intensity(1.0 + index * .3)
    fill = spawn(unreal.PointLight, "FillLight", (250, -200, 450))
    fill.light_component.set_mobility(unreal.ComponentMobility.MOVABLE)
    fill.light_component.set_intensity(30)
    fill.light_component.set_editor_property("attenuation_radius", 2500)
    volume = spawn(unreal.PostProcessVolume, "CaptureLightingProfile", (0, 0, 0))
    volume.set_editor_property("unbound", True)
    settings = volume.get_editor_property("settings")
    for field, value in dict(dynamic_global_illumination_method=unreal.DynamicGlobalIlluminationMethod.NONE, reflection_method=unreal.ReflectionMethod.NONE, ambient_occlusion_intensity=0.0, bloom_intensity=0.0, motion_blur_amount=0.0).items():
        settings.set_editor_property("override_" + field, True)
        settings.set_editor_property(field, value)
    volume.set_editor_property("settings", settings)
    unreal.EditorLevelLibrary.save_current_level()
    names = [f"Train{i:02d}" for i in range(1, 9)] + ["Validation01"] if index < 3 else [f"Test{i:02d}" for i in range(1, 4)]
    for i, sequence_name in enumerate(names):
        sequence(name, sequence_name, camera, mover, phase=i * .63 + index * .37)


def main():
    glass = material("M_Glass", color=(.1, .5, .65), roughness=.15, glass=True)
    wave = material("M_Wave", color=(.06, .5, .12), roughness=.5, wpo=True)
    names = ("Gallery", "Courtyard", "Workshop", "AtriumTest")
    for index, name in enumerate(names):
        surface = material("M_Surface" + str(index), tex=texture(index))
        accent = material("M_Accent" + str(index), color=((.55, .12, .06), (.12, .34, .18), (.15, .18, .5), (.65, .42, .07))[index], roughness=.15 + index * .08, metallic=.5)
        scene(index, name, surface, accent, glass, wave)
    content = Path(unreal.Paths.project_content_dir()) / "SRFormalV1"
    files = {p.relative_to(content).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest() for p in sorted(content.rglob("*")) if p.is_file()}
    (SOURCE / "assets.json").write_text(json.dumps({"schema": "sr-formal-assets-v1", "root": ROOT, "maps": list(names), "generatorSha256": hashlib.sha256(Path(__file__).read_bytes()).hexdigest(), "filesSha256": files}, indent=2), encoding="utf-8")
    unreal.log("Formal dataset assets complete: " + ROOT)


main()
