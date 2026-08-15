"""Generate deterministic validation-only content shipped with the plugin.

Run through UnrealEditor-Cmd with the PythonScriptPlugin enabled. The generated
material exposes explicit current/previous world-space WPO parameters and uses
PreviousFrameSwitch, so endpoint motion is produced by UE's velocity pass rather
than reconstructed by an offline approximation. A plugin-owned Niagara system is
also built from UE's deterministic Grid Location emitter template so the runtime
gate never depends on content from the host project.
"""

from __future__ import annotations

import sys

import unreal


PACKAGE_PATH = "/SuperResolutionDataset/Validation"
WPO_ASSET_NAME = "M_SRDatasetWPOFixture"
WPO_ASSET_PATH = f"{PACKAGE_PATH}/{WPO_ASSET_NAME}"
VFX_MATERIAL_ASSET_NAME = "M_SRDatasetVFXSpriteFixture"
VFX_MATERIAL_ASSET_PATH = f"{PACKAGE_PATH}/{VFX_MATERIAL_ASSET_NAME}"
NIAGARA_EMITTER_TEMPLATE_PATH = (
    "/Niagara/DefaultAssets/Templates/BehaviorExamples/GridLocation.GridLocation"
)
NIAGARA_ASSET_NAME = "NS_SRDatasetVFXFixture"
NIAGARA_ASSET_PATH = f"{PACKAGE_PATH}/{NIAGARA_ASSET_NAME}"
NIAGARA_GPU_ASSET_NAME = "NS_SRDatasetGPUVFXFixture"
NIAGARA_GPU_ASSET_PATH = f"{PACKAGE_PATH}/{NIAGARA_GPU_ASSET_NAME}"


def has_command_line_flag(flag: str) -> bool:
    return flag in sys.argv or flag.lower() in unreal.SystemLibrary.get_command_line().lower()


def require(value: bool, message: str) -> None:
    if not value:
        raise RuntimeError(message)


def create_expression(material, expression_class, x: int, y: int):
    expression = unreal.MaterialEditingLibrary.create_material_expression(
        material, expression_class, x, y
    )
    if expression is None:
        raise RuntimeError(f"Could not create {expression_class.__name__}")
    return expression


def generate_wpo_material() -> None:
    existing_material = unreal.load_asset(WPO_ASSET_PATH)
    if existing_material is not None and not has_command_line_flag("--replace-wpo"):
        unreal.log(
            f"Preserving existing WPO validation material: {WPO_ASSET_PATH} "
            "(pass --replace-wpo to rebuild it)"
        )
        return
    if existing_material is not None:
        require(
            unreal.EditorAssetLibrary.delete_asset(WPO_ASSET_PATH),
            f"Could not replace generated asset {WPO_ASSET_PATH}",
        )

    material = unreal.AssetToolsHelpers.get_asset_tools().create_asset(
        WPO_ASSET_NAME,
        PACKAGE_PATH,
        unreal.Material,
        unreal.MaterialFactoryNew(),
    )
    if material is None:
        raise RuntimeError(f"Could not create {WPO_ASSET_PATH}")

    material.set_editor_property("blend_mode", unreal.BlendMode.BLEND_OPAQUE)
    material.set_editor_property(
        "shading_model", unreal.MaterialShadingModel.MSM_UNLIT
    )
    material.set_editor_property("two_sided", True)
    material.set_editor_property("always_evaluate_world_position_offset", True)

    current_offset = create_expression(
        material, unreal.MaterialExpressionVectorParameter, -900, -180
    )
    current_offset.set_editor_property(
        "parameter_name", "SRDatasetWPOCurrentWorldCm"
    )
    current_offset.set_editor_property("default_value", unreal.LinearColor(0, 0, 0, 0))

    previous_offset = create_expression(
        material, unreal.MaterialExpressionVectorParameter, -900, 40
    )
    previous_offset.set_editor_property(
        "parameter_name", "SRDatasetWPOPreviousWorldCm"
    )
    previous_offset.set_editor_property("default_value", unreal.LinearColor(0, 0, 0, 0))

    previous_frame_switch = create_expression(
        material, unreal.MaterialExpressionPreviousFrameSwitch, -500, -100
    )
    require(
        unreal.MaterialEditingLibrary.connect_material_expressions(
            current_offset, "", previous_frame_switch, "Current Frame"
        ),
        "Could not connect the current-frame WPO branch",
    )
    require(
        unreal.MaterialEditingLibrary.connect_material_expressions(
            previous_offset, "", previous_frame_switch, "Previous Frame"
        ),
        "Could not connect the previous-frame WPO branch",
    )
    require(
        unreal.MaterialEditingLibrary.connect_material_property(
            previous_frame_switch,
            "",
            unreal.MaterialProperty.MP_WORLD_POSITION_OFFSET,
        ),
        "Could not connect World Position Offset",
    )

    fixture_color = create_expression(
        material, unreal.MaterialExpressionConstant3Vector, -450, 190
    )
    fixture_color.set_editor_property(
        "constant", unreal.LinearColor(0.02, 0.8, 0.18, 1.0)
    )
    require(
        unreal.MaterialEditingLibrary.connect_material_property(
            fixture_color, "", unreal.MaterialProperty.MP_EMISSIVE_COLOR
        ),
        "Could not connect fixture emissive color",
    )

    unreal.MaterialEditingLibrary.layout_material_expressions(material)
    unreal.MaterialEditingLibrary.recompile_material(material)
    require(
        unreal.EditorAssetLibrary.save_loaded_asset(material, False),
        f"Could not save {WPO_ASSET_PATH}",
    )
    unreal.log(f"Generated deterministic WPO validation material: {WPO_ASSET_PATH}")


def generate_vfx_material() -> None:
    existing_material = unreal.load_asset(VFX_MATERIAL_ASSET_PATH)
    if (
        existing_material is not None
        and not has_command_line_flag("--replace-vfx-material")
    ):
        existing_material.set_editor_property("used_with_niagara_sprites", True)
        existing_material.set_editor_property("used_with_niagara_ribbons", True)
        existing_material.set_editor_property(
            "translucency_pass", unreal.MaterialTranslucencyPass.MTP_AFTER_DOF
        )
        # Changing TranslucencyPass affects mesh-pass shader compilation. Saving
        # the package alone can leave the already-compiled default (Before DOF)
        # resource active in command-line game captures, even though the editor
        # property serializes as After DOF.
        unreal.MaterialEditingLibrary.recompile_material(existing_material)
        require(
            unreal.EditorAssetLibrary.save_loaded_asset(existing_material, False),
            f"Could not update usage flags on {VFX_MATERIAL_ASSET_PATH}",
        )
        unreal.log(
            f"Preserving existing Niagara sprite validation material: "
            f"{VFX_MATERIAL_ASSET_PATH} (pass --replace-vfx-material to rebuild it)"
        )
        return
    if existing_material is not None:
        require(
            unreal.EditorAssetLibrary.delete_asset(VFX_MATERIAL_ASSET_PATH),
            f"Could not replace generated asset {VFX_MATERIAL_ASSET_PATH}",
        )
    material = unreal.AssetToolsHelpers.get_asset_tools().create_asset(
        VFX_MATERIAL_ASSET_NAME,
        PACKAGE_PATH,
        unreal.Material,
        unreal.MaterialFactoryNew(),
    )
    if material is None:
        raise RuntimeError(f"Could not create {VFX_MATERIAL_ASSET_PATH}")
    material.set_editor_property("blend_mode", unreal.BlendMode.BLEND_TRANSLUCENT)
    material.set_editor_property(
        "shading_model", unreal.MaterialShadingModel.MSM_UNLIT
    )
    material.set_editor_property("two_sided", True)
    material.set_editor_property("used_with_niagara_sprites", True)
    material.set_editor_property("used_with_niagara_ribbons", True)
    material.set_editor_property(
        "translucency_pass", unreal.MaterialTranslucencyPass.MTP_AFTER_DOF
    )

    emissive = create_expression(
        material, unreal.MaterialExpressionConstant3Vector, -350, -80
    )
    emissive.set_editor_property("constant", unreal.LinearColor(5.0, 0.02, 2.0, 1.0))
    opacity = create_expression(material, unreal.MaterialExpressionConstant, -350, 80)
    opacity.set_editor_property("r", 0.85)
    require(
        unreal.MaterialEditingLibrary.connect_material_property(
            emissive, "", unreal.MaterialProperty.MP_EMISSIVE_COLOR
        ),
        "Could not connect VFX fixture emissive color",
    )
    require(
        unreal.MaterialEditingLibrary.connect_material_property(
            opacity, "", unreal.MaterialProperty.MP_OPACITY
        ),
        "Could not connect VFX fixture opacity",
    )
    unreal.MaterialEditingLibrary.layout_material_expressions(material)
    unreal.MaterialEditingLibrary.recompile_material(material)
    require(
        unreal.EditorAssetLibrary.save_loaded_asset(material, False),
        f"Could not save {VFX_MATERIAL_ASSET_PATH}",
    )
    unreal.log(f"Generated Niagara sprite validation material: {VFX_MATERIAL_ASSET_PATH}")


def generate_niagara_system(asset_path: str, use_gpu_simulation: bool) -> None:
    # Build from a deterministic grid emitter instead of duplicating a system
    # template whose modules explicitly opt into non-deterministic random
    # distributions. A small editor-only C++ bridge adds the emitter because
    # FNiagaraEmitterHandle is not exposed to Unreal Python.
    template = unreal.load_asset(NIAGARA_EMITTER_TEMPLATE_PATH)
    require(
        template is not None,
        f"Missing built-in Niagara emitter template {NIAGARA_EMITTER_TEMPLATE_PATH}",
    )
    existing_system = unreal.load_asset(asset_path)
    if existing_system is not None and not has_command_line_flag("--replace-niagara"):
        unreal.log(
            f"Preserving existing Niagara validation system: {asset_path} "
            "(pass --replace-niagara to rebuild it)"
        )
        return
    if existing_system is not None:
        require(
            unreal.EditorAssetLibrary.delete_asset(asset_path),
            f"Could not replace generated asset {asset_path}",
        )
    generation_result = unreal.SRDatasetBlueprintLibrary.generate_validation_niagara_system_asset(
        asset_path,
        NIAGARA_EMITTER_TEMPLATE_PATH,
        1337,
        use_gpu_simulation,
    )
    unreal.log(
        f"Validation Niagara editor bridge result: {generation_result!r} "
        f"({type(generation_result)!r})"
    )
    if isinstance(generation_result, tuple):
        generated, generation_error = generation_result
    elif isinstance(generation_result, str):
        # Unreal Python exposes a sole FString out parameter directly and omits
        # the native bool return value. Empty OutError means the bridge succeeded.
        generated, generation_error = not generation_result, generation_result
    else:
        generated, generation_error = bool(generation_result), "unknown editor bridge error"
    require(generated, generation_error)
    system = unreal.load_asset(asset_path)
    if system is None:
        raise RuntimeError(
            f"Could not load generated Niagara system {asset_path}"
        )
    # These properties are consumed when a component instance is initialized.
    # Runtime capture also enforces and restores them for host systems.
    system.set_editor_property("determinism", True)
    system.set_editor_property("fixed_tick_delta", True)
    system.set_editor_property("random_seed", 1337)
    system.set_editor_property("fixed_tick_delta_time", 1.0 / 30.0)
    require(
        unreal.EditorAssetLibrary.save_loaded_asset(system, False),
        f"Could not save {asset_path}",
    )
    unreal.log(
        "Generated deterministic Niagara validation system: "
        f"{asset_path} from {NIAGARA_EMITTER_TEMPLATE_PATH}; "
        f"sim_target={'GPUComputeSim' if use_gpu_simulation else 'CPUSim'}"
    )


def generate() -> None:
    unreal.EditorAssetLibrary.make_directory(PACKAGE_PATH)
    generate_wpo_material()
    generate_vfx_material()
    generate_niagara_system(NIAGARA_ASSET_PATH, False)
    generate_niagara_system(NIAGARA_GPU_ASSET_PATH, True)


if __name__ == "__main__":
    generate()
