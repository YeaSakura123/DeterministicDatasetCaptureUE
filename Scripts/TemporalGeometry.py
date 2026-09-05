"""Independent geometry for saved raster validation (row-vector UE matrices)."""
import numpy as np

HISTORY_V3 = "component_identity_and_static_camera_depth_on_jittered_rasters_v3"


def matrix_jitter_raster_offset(current, previous, render_size):
    """Recover input-grid offset by projecting a probe, without jitter fields."""
    def project_between(source, target):
        probe = np.array([0.0, 0.0, 0.5, 1.0])
        result = probe @ np.linalg.inv(np.asarray(source).reshape(4, 4)) @ np.asarray(target).reshape(4, 4)
        return result[:2] / result[3]

    current_to_fixed = project_between(current["viewToClipCurrentJittered"], current["viewToClipCurrentUnjittered"])
    fixed_to_previous = project_between(previous["viewToClipCurrentUnjittered"], previous["viewToClipCurrentJittered"])
    width, height = render_size
    return (current_to_fixed + fixed_to_previous) * np.array([width / 2, -height / 2])


def previous_view_matches_saved(current, previous):
    """A matching jitter phase alone cannot identify the previous logical frame."""
    def origin(metadata, suffix):
        return np.asarray(metadata[f"worldViewOriginHigh{suffix}"], dtype=np.float64) + np.asarray(metadata[f"worldViewOriginLow{suffix}"], dtype=np.float64)

    origin_error = float(np.max(np.abs(origin(current, "Previous") - origin(previous, "Current"))))
    view_error = float(np.max(np.abs(np.asarray(current["translatedWorldToViewPrevious"]) - np.asarray(previous["translatedWorldToViewCurrent"]))))
    projection_error = float(np.max(np.abs(np.asarray(current["viewToClipPreviousUnjittered"]) - np.asarray(previous["viewToClipCurrentUnjittered"]))))
    return (origin_error <= 1e-3 and view_error <= 1e-5 and projection_error <= 1e-6), dict(originErrorCm=origin_error, viewMatrixError=view_error, projectionError=projection_error)
