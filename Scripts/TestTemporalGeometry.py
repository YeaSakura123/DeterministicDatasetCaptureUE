"""Regression tests with projective ground truth, independent of capture code."""
import unittest
import numpy as np
from TemporalGeometry import matrix_jitter_raster_offset, previous_view_matches_saved


def projection(jitter):
    return np.array([[1.0, 0, 0, 0], [0, 16/9, 0, 0], [jitter[0], jitter[1], 0, 1], [0, 0, 5, 0]])


class TemporalGeometryTests(unittest.TestCase):
    def test_jittered_correspondence_matches_projected_world_points(self):
        width, height = 256, 144
        points = np.array([[0, 0, 100, 1], [12, -7, 800, 1], [-45, 22, 1000, 1]])
        phases = [(0, -1/6), (-.25, 1/6), (.25, -7/18), (-.375, -1/18)]
        for previous_jitter, current_jitter in zip(phases, phases[1:]):
            to_ndc = np.array([2/width, -2/height])
            current_p = projection(np.array(current_jitter) * to_ndc)
            previous_p = projection(np.array(previous_jitter) * to_ndc)
            current = dict(viewToClipCurrentJittered=current_p, viewToClipCurrentUnjittered=projection((0, 0)))
            previous = dict(viewToClipCurrentJittered=previous_p, viewToClipCurrentUnjittered=projection((0, 0)))
            c, p = points @ current_p, points @ previous_p
            expected = (p[:, :2] / p[:, 3:4] - c[:, :2] / c[:, 3:4]) * np.array([width/2, -height/2])
            actual = matrix_jitter_raster_offset(current, previous, (width, height))
            np.testing.assert_allclose(np.broadcast_to(actual, expected.shape), expected, atol=1e-12)

    def test_equal_jitter_cannot_hide_wrong_previous_camera(self):
        identity = np.eye(4).ravel().tolist()
        previous = dict(worldViewOriginHighCurrent=[0, 0, 200], worldViewOriginLowCurrent=[0, 0, 0],
                        translatedWorldToViewCurrent=identity, viewToClipCurrentUnjittered=projection((0, 0)).ravel().tolist())
        current = dict(worldViewOriginHighPrevious=[0, 20, 200], worldViewOriginLowPrevious=[0, 0, 0],
                       translatedWorldToViewPrevious=identity, viewToClipPreviousUnjittered=projection((0, 0)).ravel().tolist())
        matched, metrics = previous_view_matches_saved(current, previous)
        self.assertFalse(matched)
        self.assertEqual(metrics['originErrorCm'], 20)
        current['worldViewOriginHighPrevious'] = previous['worldViewOriginHighCurrent']
        self.assertTrue(previous_view_matches_saved(current, previous)[0])


if __name__ == '__main__':
    unittest.main()
