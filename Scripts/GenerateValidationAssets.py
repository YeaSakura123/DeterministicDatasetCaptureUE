"""Generate deterministic validation-only content shipped with the plugin.

Run through UnrealEditor-Cmd with the PythonScriptPlugin enabled. The generated
material exposes explicit current/previous world-space WPO parameters and uses
PreviousFrameSwitch, so endpoint motion is produced by UE's velocity pass rather
than reconstructed by an offline approximation.
"""

from __future__ import annotations

import unreal


PACKAGE_PATH = "/SuperResolutionDataset/Validation"
ASSET_NAME = "M_SRDatasetWPOFixture"
ASSET_PATH = f"{PACKAGE_PATH}/{ASSET_NAME}"


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


def generate() -> None:
    unreal.EditorAssetLibrary.make_directory(PACKAGE_PATH)
    if unreal.EditorAssetLibrary.does_asset_exist(ASSET_PATH):
        require(
            unreal.EditorAssetLibrary.delete_asset(ASSET_PATH),
            f"Could not replace generated asset {ASSET_PATH}",
        )

    material = unreal.AssetToolsHelpers.get_asset_tools().create_asset(
        ASSET_NAME,
        PACKAGE_PATH,
        unreal.Material,
        unreal.MaterialFactoryNew(),
    )
    if material is None:
        raise RuntimeError(f"Could not create {ASSET_PATH}")

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
        f"Could not save {ASSET_PATH}",
    )
    unreal.log(f"Generated deterministic WPO validation material: {ASSET_PATH}")


if __name__ == "__main__":
    generate()
