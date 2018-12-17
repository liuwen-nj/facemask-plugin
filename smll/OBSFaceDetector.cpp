/*
* Face Masks for SlOBS
* smll - streamlabs machine learning library
*
* Copyright (C) 2017 General Workings Inc
*
* This program is free software; you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation; either version 2 of the License, or
* (at your option) any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with this program; if not, write to the Free Software
* Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA
*/

#include "OBSFaceDetector.hpp"


#define HULL_POINTS_SCALE		(1.25f)
// border points = 4 corners + subdivide
#define NUM_BORDER_POINTS		(4 * 2 * 2 * 2) 
#define NUM_BORDER_POINT_DIVS	(3)
// hull points = head + jaw + subdivide
#define NUM_HULL_POINTS			(28 * 2 * 2 * 2)
#define NUM_HULL_POINT_DIVS		(3)

static const char* const kFileShapePredictor68 = "shape_predictor_68_face_landmarks.dat";


using namespace dlib;
using namespace std;


namespace smll {

	OBSFaceDetector::OBSFaceDetector()
		: m_captureStage(nullptr)
		, m_stageSize(0)
		, m_timeout(0)
        , m_trackingTimeout(0)
        , m_detectionTimeout(0)
		, m_trackingFaceIndex(0)
		, m_camera_w(0)
		, m_camera_h(0) {

		// Init DLIB's shape predictor.
		char *filename = obs_module_file(kFileShapePredictor68);
		_faceLandmarks.Init(filename);
		bfree(filename);
	}

	OBSFaceDetector::~OBSFaceDetector() {
		obs_enter_graphics();
		if (m_captureStage)
			gs_stagesurface_destroy(m_captureStage);
		obs_leave_graphics();
	}


	void OBSFaceDetector::MakeVtxBitmaskLookup() {
		if (m_vtxBitmaskLookup.size() == 0) {
			for (int i = 0; i < NUM_MORPH_LANDMARKS; i++) {
				LandmarkBitmask b;
				b.set(i);
				m_vtxBitmaskLookup.push_back(b);
			}
			for (int i = 0; i < NUM_FACE_CONTOURS; i++) {
				const FaceContour& fc = GetFaceContour((FaceContourID)i);
				for (int j = 0; j < fc.num_smooth_points; j++) {
					m_vtxBitmaskLookup.push_back(fc.bitmask);
				}
			}
			LandmarkBitmask bp, hp;
			bp.set(BORDER_POINT);
			hp.set(HULL_POINT);
			for (int i = 0; i < NUM_BORDER_POINTS; i++) {
				m_vtxBitmaskLookup.push_back(bp);
			}
			for (int i = 0; i < NUM_HULL_POINTS; i++) {
				m_vtxBitmaskLookup.push_back(hp);
			}
		}
	}

	const cv::Mat& OBSFaceDetector::GetCVCamMatrix() {
		SetCVCamera();
		return m_camera_matrix;
	}

	const cv::Mat& OBSFaceDetector::GetCVDistCoeffs() {
		SetCVCamera();
		return m_dist_coeffs;
	}

	void OBSFaceDetector::SetCVCamera() {
		int w = CaptureWidth();
		int h = CaptureHeight();

		if (m_camera_w != w || m_camera_h != h) {
			m_camera_w = w;
			m_camera_h = h;

			// Approximate focal length.
			float focal_length = (float)m_camera_w;
			cv::Point2f center = cv::Point2f(m_camera_w / 2.0f, m_camera_h / 2.0f);
			m_camera_matrix =
				(cv::Mat_<float>(3, 3) <<
					focal_length, 0,			center.x, 
					0,			  focal_length, center.y, 
					0,			  0,			1);
			// We assume no lens distortion
			m_dist_coeffs = cv::Mat::zeros(4, 1, cv::DataType<float>::type);
		}
	}

	void OBSFaceDetector::computeCurrentImage(const ImageWrapper& detect) {
		// Do image cropping and cv::Mat initialization in single shot
		CropInfo cropInfo = GetCropInfo();

		char* cropData = detect.data +
			(detect.getStride() * cropInfo.offsetY) +
			(detect.getNumElems() * cropInfo.offsetX);

		cv::Mat gray(cropInfo.height, cropInfo.width, CV_8UC1, cropData, m_detect.getStride());
		currentImage = gray;
	}

	void OBSFaceDetector::DetectFaces(const ImageWrapper& detect, const OBSTexture& capture, DetectionResults& results) {

		// Wait for CONFIG_INT_FACE_DETECT_FREQUENCY after all faces are lost before trying to detect them again
		if (m_timeout > 0) {
			m_timeout--;
			return;
		}

		// better check if the camera res has changed on us
		if ((detect.w != m_detect.w) ||
			(detect.h != m_detect.h)) {
			// forget whatever we thought were faces
			m_faces.length = 0;
		}

		// save detect for convenience
		m_detect = detect;
		m_capture = capture;
		// Compute GrayScale image.
		// This will be used for the rest of the Computer Vision.
		computeCurrentImage(detect);

		bool trackingFailed = false;
		// if number of frames before the last detection is bigger than the threshold or if there are no faces to track
		if (m_detectionTimeout == 0 || m_faces.length == 0) {
			DoFaceDetection();
			m_detectionTimeout =
				Config::singleton().get_int(CONFIG_INT_FACE_DETECT_RECHECK_FREQUENCY);
			StartObjectTracking();
		}
		else if (m_trackingTimeout == 0) {
			m_detectionTimeout--;

			UpdateObjectTracking();

			// Is Tracking is still good?
			if (m_faces.length > 0) {
				// next face for tracking time-slicing
				m_trackingFaceIndex = (m_trackingFaceIndex + 1) % m_faces.length;

				// tracking frequency
				m_trackingTimeout =
					Config::singleton().get_int(CONFIG_INT_TRACKING_FREQUNCY);
			}
			else {
				m_trackingFaceIndex = 0;
				// force detection on the next frame, do not wait for 5 frames
				m_timeout == 0;
				trackingFailed = true;
			}

			// copy faces to results
			for (int i = 0; i < m_faces.length; i++) {
				results[i] = m_faces[i];
			}
			results.length = m_faces.length;
		}
		else
		{
			m_detectionTimeout--;
			m_trackingTimeout--;
		}

		// copy faces to results
		for (int i = 0; i < m_faces.length; i++) {
			results[i] = m_faces[i];
		}
		results.length = m_faces.length;

		// If faces are not found
		if (m_faces.length == 0 && !trackingFailed) {
            // Wait for 5 frames and do face detection
            m_timeout = Config::singleton().get_int(CONFIG_INT_FACE_DETECT_FREQUENCY);
		}
	}

	void OBSFaceDetector::MakeTriangulation(MorphData& morphData, 
		DetectionResults& results,
		TriangulationResult& result) {

		// clear buffers
		result.DestroyBuffers();

		// need valid morph data
		if (!morphData.IsValid())
			return;

		// make sure we have our bitmask lookup table
		MakeVtxBitmaskLookup();

		// only 1 face supported (for now)
		if (results.length == 0)
			return;
		DetectionResult& face = results[0];

		// get angle of face pose
		const cv::Mat& m = face.GetCVRotation();
		double angle = sqrt(m.dot(m));
		bool faceDeadOn = (angle < 0.1f);

		// save capture width and height
		float width = (float)CaptureWidth();
		float height = (float)CaptureHeight();

		// Project morph deltas to image space

		// project the deltas
		// TODO: only project non-zero deltas 
		//       (although, to be honest, with compiler opts and such, it
		//        would likely take longer to separate them out)
		const cv::Mat& rot = face.GetCVRotation();
		cv::Mat trx;
		face.GetCVTranslation().copyTo(trx); // make sure to copy!
		trx.at<double>(0, 0) = 0.0; // clear x & y, we'll center it
		trx.at<double>(1, 0) = 0.0;
		std::vector<cv::Point3f> deltas = morphData.GetCVDeltas();
		std::vector<cv::Point2f> projectedDeltas;
		cv::projectPoints(deltas, rot, trx, GetCVCamMatrix(), GetCVDistCoeffs(), projectedDeltas);

		// make a list of points for triangulation
		std::vector<cv::Point2f> points;

		// add facial landmark points
		dlib::point* facePoints = face.landmarks68;
		for (int i = 0; i < NUM_FACIAL_LANDMARKS; i++) {
			points.push_back(cv::Point2f((float)facePoints[i].x(),
				(float)facePoints[i].y()));
		}

		// add the head points
		AddHeadPoints(points, face);

		// Apply the morph deltas to points to create warpedpoints
		std::vector<cv::Point2f> warpedpoints = points;
		cv::Point2f c(width / 2, height / 2);
		for (int i = 0; i < NUM_MORPH_LANDMARKS; i++) {
			// bitmask tells us which deltas are non-zero
			if (morphData.GetBitmask()[i]) {
				// offset from center
				warpedpoints[i] += projectedDeltas[i] - c;
			}
		}

		// add smoothing points
		for (int i = 0; i < NUM_FACE_CONTOURS; i++) {
			const FaceContour& fc = GetFaceContour((FaceContourID)i);
			// smooth em out
			CatmullRomSmooth(points, fc.indices, NUM_SMOOTHING_STEPS);
			CatmullRomSmooth(warpedpoints, fc.indices, NUM_SMOOTHING_STEPS);
		}

		// add border points
		std::vector<cv::Point2f> borderpoints;
		// 4 corners
		borderpoints.push_back(cv::Point2f(0, 0));
		borderpoints.push_back(cv::Point2f(width, 0));
		borderpoints.push_back(cv::Point2f(width, height));
		borderpoints.push_back(cv::Point2f(0, height));
		// subdivide
		for (int i = 0; i < NUM_BORDER_POINT_DIVS; i++) {
			Subdivide(borderpoints);
		}
		points.insert(points.end(), borderpoints.begin(), borderpoints.end());
		warpedpoints.insert(warpedpoints.end(), borderpoints.begin(), borderpoints.end());

		// add hull points
		std::vector<cv::Point2f> hullpoints;
		MakeHullPoints(points, warpedpoints, hullpoints);
		points.reserve(points.size() + hullpoints.size());
		warpedpoints.reserve(warpedpoints.size() + hullpoints.size());
		for (int i = 0; i < hullpoints.size(); i++) {
			points.push_back(hullpoints[i]);
			warpedpoints.push_back(hullpoints[i]);
		}

		// make the vertex buffer 
		// TODO: we should probably leave the creation of this
		//       graphics stuff to the render method
		//
		obs_enter_graphics();
		gs_render_start(true);
		size_t nv = points.size();
		LandmarkBitmask hpbm;
		hpbm.set(HULL_POINT);
		for (int i = 0; i < nv; i++) {
			// position from warped points
			// uv from original points
			const cv::Point2f& p = warpedpoints[i];
			const cv::Point2f& uv = points[i];

			// add point and uv
			gs_texcoord(uv.x / width, uv.y / height, 0);
			gs_vertex2f(p.x, p.y);

			if ((m_vtxBitmaskLookup[i] & hpbm).any())
				gs_color(0x0);
			else
				gs_color(0xFFFFFFFF);
		}
		result.vertexBuffer = gs_render_save();
		obs_leave_graphics();


		// Create Triangulation

		// create the openCV Subdiv2D object
		cv::Rect rect(0, 0, CaptureWidth() + 1, CaptureHeight() + 1);
		cv::Subdiv2D subdiv(rect);

		// add our points to subdiv2d
		// save a map: subdiv2d vtx id -> index into our points
		std::map<int, int> vtxMap;
		size_t nsmooth = GetFaceContour(FACE_CONTOUR_LAST).smooth_points_index +
			GetFaceContour(FACE_CONTOUR_LAST).num_smooth_points;
		LandmarkBitmask facebm = TriangulationResult::GetBitmasks()[TriangulationResult::IDXBUFF_FACE];
		if (faceDeadOn) {
			// don't bother doing boundary checks if face is dead-on
			facebm = facebm | TriangulationResult::GetBitmasks()[TriangulationResult::IDXBUFF_LINES];
		}
		for (int i = 0; i < points.size(); i++) {
			cv::Point2f& p = points[i];
			// only add points belonging to face, hull, border
			if ((i >= nsmooth || (m_vtxBitmaskLookup[i] & facebm).any())
				&& rect.contains(p)) {
				// note: this crashes if you insert a point outside the rect.
				try {
					int vid = subdiv.insert(p);
					vtxMap[vid] = i;
				}
				catch (const std::exception& e) {
					blog(LOG_DEBUG, "[FaceMask] ***** CAUGHT EXCEPTION CV::SUBDIV2D: %s", e.what());
					blog(LOG_DEBUG, "[FaceMask] ***** whilst adding point %d at (%f,%f)", i, p.x, p.y);
				}
			}
		}

		// selectively add eyebrows & nose points
		if (!faceDeadOn) {
			AddSelectivePoints(subdiv, points, warpedpoints, vtxMap);
		}

		// get triangulation
		std::vector<cv::Vec3i>	triangleList;
		subdiv.getTriangleIndexList(triangleList);

		// NOTE: openCV's subdiv2D class adds 4 points on initialization:
		//
		//       p0 = 0,0
		//       p1 = M,0
		//       p2 = 0,M
		//       p3 = -M,-M
		//
		// where M = max(W,H) * 3
		//
		// I assume this is to ensure the entire triangulation is contained 
		// in a triangle. Or something. I dunno.
		// Either way, since I add my own border points myself, these first
		// 4 vertices are crap to us, and all resulting triangles are also
		// crap.
		// We ignore these triangles, and use a vtxMap we created above
		// to re-index the triangle indices to our vtx list.
		//
		// Also, the subdiv2D object will merge vertices that are close enough
		// to each other. The vtxMap also fixes this, and we use the 
		// revVtxMap for area sorting later on.

		// re-index triangles and remove bad ones
		for (int i = 0; i < triangleList.size(); i++) {
			cv::Vec3i &t = triangleList[i];
			if (t[0] < 4 || t[1] < 4 || t[2] < 4) {
				// mark triangle as bad
				t[0] = 0;
				t[1] = 0;
				t[2] = 0;
			}
			else {
				// re-index
				t[0] = vtxMap[t[0]];
				t[1] = vtxMap[t[1]];
				t[2] = vtxMap[t[2]];
			}
		}

		// Make Index Buffers
		MakeAreaIndices(result, triangleList);
	}


	void OBSFaceDetector::AddSelectivePoints(cv::Subdiv2D& subdiv,
		const std::vector<cv::Point2f>& points,
		const std::vector<cv::Point2f>& warpedpoints, std::map<int, int>& vtxMap) {

		bool turnedLeft = warpedpoints[NOSE_4].x < warpedpoints[NOSE_1].x;

		if (turnedLeft) {
			AddContourSelective(subdiv, GetFaceContour(FACE_CONTOUR_EYEBROW_LEFT), points, warpedpoints, vtxMap, true);
			AddContourSelective(subdiv, GetFaceContour(FACE_CONTOUR_EYE_LEFT_TOP), points, warpedpoints, vtxMap, true);
			AddContourSelective(subdiv, GetFaceContour(FACE_CONTOUR_EYE_LEFT_BOTTOM), points, warpedpoints, vtxMap, true);
			AddContourSelective(subdiv, GetFaceContour(FACE_CONTOUR_EYE_LEFT_BOTTOM), points, warpedpoints, vtxMap, true);
			AddContourSelective(subdiv, GetFaceContour(FACE_CONTOUR_MOUTH_OUTER_TOP_LEFT), points, warpedpoints, vtxMap, true);

			AddContour(subdiv, GetFaceContour(FACE_CONTOUR_EYEBROW_RIGHT), points, vtxMap);
			AddContour(subdiv, GetFaceContour(FACE_CONTOUR_EYE_RIGHT_TOP), points, vtxMap);
			AddContour(subdiv, GetFaceContour(FACE_CONTOUR_EYE_RIGHT_BOTTOM), points, vtxMap);
			AddContour(subdiv, GetFaceContour(FACE_CONTOUR_EYE_RIGHT_BOTTOM), points, vtxMap);
			AddContour(subdiv, GetFaceContour(FACE_CONTOUR_MOUTH_OUTER_TOP_RIGHT), points, vtxMap);
		}
		else {
			AddContourSelective(subdiv, GetFaceContour(FACE_CONTOUR_EYEBROW_RIGHT), points, warpedpoints, vtxMap, false);
			AddContourSelective(subdiv, GetFaceContour(FACE_CONTOUR_EYE_RIGHT_TOP), points, warpedpoints, vtxMap, false);
			AddContourSelective(subdiv, GetFaceContour(FACE_CONTOUR_EYE_RIGHT_BOTTOM), points, warpedpoints, vtxMap, false);
			AddContourSelective(subdiv, GetFaceContour(FACE_CONTOUR_EYE_RIGHT_BOTTOM), points, warpedpoints, vtxMap, false);
			AddContourSelective(subdiv, GetFaceContour(FACE_CONTOUR_MOUTH_OUTER_TOP_RIGHT), points, warpedpoints, vtxMap, false);

			AddContour(subdiv, GetFaceContour(FACE_CONTOUR_EYEBROW_LEFT), points, vtxMap);
			AddContour(subdiv, GetFaceContour(FACE_CONTOUR_EYE_LEFT_TOP), points, vtxMap);
			AddContour(subdiv, GetFaceContour(FACE_CONTOUR_EYE_LEFT_BOTTOM), points, vtxMap);
			AddContour(subdiv, GetFaceContour(FACE_CONTOUR_EYE_LEFT_BOTTOM), points, vtxMap);
			AddContour(subdiv, GetFaceContour(FACE_CONTOUR_MOUTH_OUTER_TOP_LEFT), points, vtxMap);
		}

		AddContourSelective(subdiv, GetFaceContour(FACE_CONTOUR_NOSE_BRIDGE), points, warpedpoints, vtxMap, turnedLeft);
		AddContourSelective(subdiv, GetFaceContour(FACE_CONTOUR_NOSE_BOTTOM), points, warpedpoints, vtxMap, turnedLeft);
		AddContourSelective(subdiv, GetFaceContour(FACE_CONTOUR_MOUTH_OUTER_BOTTOM), points, warpedpoints, vtxMap, turnedLeft);
	}

	void OBSFaceDetector::AddContour(cv::Subdiv2D& subdiv, const FaceContour& fc, const std::vector<cv::Point2f>& points,
		std::map<int, int>& vtxMap) {

		cv::Rect rect(0, 0, CaptureWidth() + 1, CaptureHeight() + 1);

		// add points 
		for (int i = 0; i < fc.indices.size(); i++) {
			const cv::Point2f& p = points[fc.indices[i]];
			if (rect.contains(p)) {
				int vid = subdiv.insert(p);
				vtxMap[vid] = fc.indices[i];
			}
		}
		int smoothidx = fc.smooth_points_index;
		for (int i = 0; i < fc.num_smooth_points; i++, smoothidx++) {
			const cv::Point2f& p = points[smoothidx];
			if (rect.contains(p)) {
				int vid = subdiv.insert(p);
				vtxMap[vid] = smoothidx;
			}
		}
	}


	void OBSFaceDetector::AddContourSelective(cv::Subdiv2D& subdiv, const FaceContour& fc,
		const std::vector<cv::Point2f>& points,
		const std::vector<cv::Point2f>& warpedpoints, std::map<int, int>& vtxMap, 
		bool checkLeft) {

		std::array<int, 15> lhead_points = { HEAD_6, HEAD_5, HEAD_4, HEAD_3, HEAD_2, 
			HEAD_1, JAW_1, JAW_2, JAW_3, JAW_4, JAW_5, JAW_6, JAW_7, JAW_8, JAW_9};
		std::array<int, 15> rhead_points = { HEAD_6, HEAD_7, HEAD_8, HEAD_9, HEAD_10, 
			HEAD_11, JAW_17, JAW_16, JAW_15, JAW_14, JAW_13, JAW_12, JAW_11, JAW_10,
			JAW_9};

		cv::Rect rect(0, 0, CaptureWidth() + 1, CaptureHeight() + 1);

		// find min/max y of contour points
		float miny = warpedpoints[fc.indices[0]].y;
		float maxy = warpedpoints[fc.indices[0]].y;
		for (int i = 1; i < fc.indices.size(); i++) {
			const cv::Point2f& p = warpedpoints[fc.indices[i]];
			if (p.y < miny)
				miny = p.y;
			if (p.y > maxy)
				maxy = p.y;
		}

		// find hi/low points on sides of face
		std::array<int, 15>& headpoints = checkLeft ? lhead_points : rhead_points;
		int lop = 0;
		int hip = (int)headpoints.size() - 1;
		for (int i = 1; i < headpoints.size(); i++) {
			if (warpedpoints[headpoints[i]].y < miny)
				lop = i;
			else
				break;
		}
		for (int i = (int)headpoints.size() - 2; i >= 0; i--) {
			if (warpedpoints[headpoints[i]].y > maxy)
				hip = i;
			else
				break;
		}

		// hi low points
		const cv::Point2f& hi = warpedpoints[headpoints[hip]];
		const cv::Point2f& lo = warpedpoints[headpoints[lop]];

		float m = 1.0f;
		if (!checkLeft)
			m = -1.0f;

		// add points if they are inside the outside line
		for (int i = 0; i < fc.indices.size(); i++) {
			const cv::Point2f& p1 = warpedpoints[fc.indices[i]];
			float d = m * ((p1.x - lo.x) * (hi.y - lo.y) - (p1.y - lo.y) * (hi.x - lo.x));
			const cv::Point2f& p = points[fc.indices[i]];
			if (d > 10.0f && rect.contains(p)) {
				int vid = subdiv.insert(p);
				vtxMap[vid] = fc.indices[i];
			}
			else
				break;
		}
		int smoothidx = fc.smooth_points_index;
		for (int i = 0; i < fc.num_smooth_points; i++, smoothidx++) {
			const cv::Point2f& p1 = warpedpoints[smoothidx];
			float d = m * ((p1.x - lo.x) * (hi.y - lo.y) - (p1.y - lo.y) * (hi.x - lo.x));
			const cv::Point2f& p = points[smoothidx];
			if (d > 10.0f && rect.contains(p)) {
				int vid = subdiv.insert(p);
				vtxMap[vid] = smoothidx;
			}
			else
				break;
		}
	}


	void OBSFaceDetector::AddHeadPoints(std::vector<cv::Point2f>& points, const DetectionResult& face) {

		points.reserve(points.size() + HP_NUM_HEAD_POINTS);

		// get the head points
		std::vector<cv::Point3f> headpoints = GetAllHeadPoints();

		// project all the head points
		const cv::Mat& rot = face.GetCVRotation();
		cv::Mat trx = face.GetCVTranslation();

		/*
		blog(LOG_DEBUG, "HEADPOINTS-----------------------");
		blog(LOG_DEBUG, " trans: %5.2f, %5.2f, %5.2f",
			(float)trx.at<double>(0, 0),
			(float)trx.at<double>(1, 0),
			(float)trx.at<double>(2, 0));
		blog(LOG_DEBUG, "   rot: %5.2f, %5.2f, %5.2f",
			(float)rot.at<double>(0, 0),
			(float)rot.at<double>(1, 0),
			(float)rot.at<double>(2, 0));
			*/

		std::vector<cv::Point2f> projheadpoints;
		cv::projectPoints(headpoints, rot, trx, GetCVCamMatrix(), GetCVDistCoeffs(), projheadpoints);

		// select the correct points

		// HEAD_1 -> HEAD_5
		for (int i = 0, j = 0; i < 5; i++, j+=3) {
			int h0 = HP_HEAD_1 + i;
			int h1 = HP_HEAD_EXTRA_1 + j;
			int h2 = HP_HEAD_EXTRA_2 + j;

			if (projheadpoints[h0].x < projheadpoints[h1].x)
				points.push_back(projheadpoints[h0]);
			else if (projheadpoints[h1].x < projheadpoints[h2].x)
				points.push_back(projheadpoints[h1]);
			else
				points.push_back(projheadpoints[h2]);
		}
		// HEAD_6
		points.push_back(projheadpoints[HP_HEAD_6]);
		// HEAD_7 -> HEAD_11
		for (int i = 0, j = 12; i < 5; i++, j -= 3) {
			int h0 = HP_HEAD_7 + i;
			int h1 = HP_HEAD_EXTRA_3 + j;
			int h2 = HP_HEAD_EXTRA_2 + j;

			if (projheadpoints[h0].x > projheadpoints[h1].x)
				points.push_back(projheadpoints[h0]);
			else if (projheadpoints[h1].x > projheadpoints[h2].x)
				points.push_back(projheadpoints[h1]);
			else
				points.push_back(projheadpoints[h2]);
		}
	}


	// MakeHullPoints
	// - these are extra points added to the morph to smooth out the appearance,
	//   and keep the rest of the video frame from morphing with it
	//
	void OBSFaceDetector::MakeHullPoints(const std::vector<cv::Point2f>& points,
		const std::vector<cv::Point2f>& warpedpoints, std::vector<cv::Point2f>& hullpoints) {
		// consider outside contours only
		const int num_contours = 2;
		const FaceContourID contours[num_contours] = { FACE_CONTOUR_CHIN, FACE_CONTOUR_HEAD };

		// find the center of the original points
		int numPoints = 0;
		cv::Point2f center(0.0f, 0.0f);
		for (int i = 0; i < num_contours; i++) {
			const FaceContour& fc = GetFaceContour(contours[i]);
			for (int j = 0; j < fc.indices.size(); j++, numPoints++) {
				center += points[fc.indices[j]];
			}
		}
		center /= (float)numPoints;

		// go through the warped points, see if they expand the hull
		// - we do this by checking the dot product of the delta to the
		//   warped point with the vector to the original point from
		//   the center
		for (int i = 0; i < num_contours; i++) {
			const FaceContour& fc = GetFaceContour(contours[i]);
			int is = 0;
			size_t ie = fc.indices.size();
			int ip = 1;
			if (fc.id == FACE_CONTOUR_HEAD) {
				// don't include jaw points twice, step backwards
				is = (int)fc.indices.size() - 2;
				ie = 0;
				ip = -1;
			}

			for (int j = is; j != ie; j += ip) {
				// get points
				const cv::Point2f&	p = points[fc.indices[j]];
				const cv::Point2f&	wp = warpedpoints[fc.indices[j]];

				// get vectors
				cv::Point2f d = wp - p;
				cv::Point2f v = p - center;
				// if dot product is positive
				if (d.dot(v) > 0) {
					// warped point expands hull
					hullpoints.push_back(wp);
				}
				else {
					// warped point shrinks hull, use original
					hullpoints.push_back(p);
				}
			}
		}

		// scale up hull points from center
		for (int i = 0; i < hullpoints.size(); i++) {
			hullpoints[i] = ((hullpoints[i] - center) * HULL_POINTS_SCALE) + center;
		}

		// subdivide
		for (int i = 0; i < NUM_BORDER_POINT_DIVS; i++) {
			Subdivide(hullpoints);
		}
	}

	// MakeAreaIndices : make index buffers for different areas of the face
	//
	void OBSFaceDetector::MakeAreaIndices(TriangulationResult& result,
		const std::vector<cv::Vec3i>& triangleList) {

		// Allocate temp storage for triangle indices
		std::vector<uint32_t> triangles[TriangulationResult::NUM_INDEX_BUFFERS];
		triangles[TriangulationResult::IDXBUFF_BACKGROUND].reserve(triangleList.size() * 3);
		triangles[TriangulationResult::IDXBUFF_FACE].reserve(triangleList.size() * 3);
		triangles[TriangulationResult::IDXBUFF_HULL].reserve(triangleList.size() * 3);
		if (result.buildLines) {
			triangles[TriangulationResult::IDXBUFF_LINES].reserve(triangleList.size() * 6);
		}

		// Sort out our triangles 
		for (int i = 0; i < triangleList.size(); i++) {
			const cv::Vec3i& tri = triangleList[i];
			int i0 = tri[0];
			int i1 = tri[1];
			int i2 = tri[2];
			if (i0 == 0 && i1 == 0 && i2 == 0) {
				// SKIP INVALID
				continue;
			}

			// lookup bitmasks
			const LandmarkBitmask& b0 = m_vtxBitmaskLookup[i0];
			const LandmarkBitmask& b1 = m_vtxBitmaskLookup[i1];
			const LandmarkBitmask& b2 = m_vtxBitmaskLookup[i2];
			
			//LandmarkBitmask bgmask = TriangulationResult::GetBitmasks()[TriangulationResult::IDXBUFF_BACKGROUND];
			LandmarkBitmask hullmask = TriangulationResult::GetBitmasks()[TriangulationResult::IDXBUFF_HULL];
			LandmarkBitmask facemask = TriangulationResult::GetBitmasks()[TriangulationResult::IDXBUFF_FACE] |
				TriangulationResult::GetBitmasks()[TriangulationResult::IDXBUFF_LINES]; // include extra points here
			LandmarkBitmask leyemask = GetFaceArea(FACE_AREA_EYE_LEFT).bitmask;
			LandmarkBitmask reyemask = GetFaceArea(FACE_AREA_EYE_RIGHT).bitmask;
			LandmarkBitmask mouthmask = GetFaceArea(FACE_AREA_MOUTH_LIPS_TOP).bitmask |
				GetFaceArea(FACE_AREA_MOUTH_LIPS_BOTTOM).bitmask;

			// one freakin' triangle! We think this is part of the mouth unless
			// we consider this one triangle made entirely of mouth points, but STILL
			// is part of the face.
			LandmarkBitmask facemask2;
			facemask2.set(MOUTH_OUTER_3);
			facemask2.set(MOUTH_OUTER_4);
			facemask2.set(MOUTH_OUTER_5);

			// remove eyes and mouth, except for the special triangle
			if ((b0 & facemask2).any() &&
				(b1 & facemask2).any() &&
				(b2 & facemask2).any()) {} // do nothing
			else if (((b0 & leyemask).any() && (b1 & leyemask).any() && (b2 & leyemask).any())||
				((b0 & reyemask).any() && (b1 & reyemask).any() && (b2 & reyemask).any()) ||
				((b0 & mouthmask).any() && (b1 & mouthmask).any() && (b2 & mouthmask).any()))
			{
				// Skip eyes and mouth
				continue;
			}

			// lines
			if (result.buildLines) {
				triangles[TriangulationResult::IDXBUFF_LINES].push_back(i0);
				triangles[TriangulationResult::IDXBUFF_LINES].push_back(i1);
				triangles[TriangulationResult::IDXBUFF_LINES].push_back(i1);
				triangles[TriangulationResult::IDXBUFF_LINES].push_back(i2);
				triangles[TriangulationResult::IDXBUFF_LINES].push_back(i2);
				triangles[TriangulationResult::IDXBUFF_LINES].push_back(i0);
			}

			if ((b0 & facemask).any() &&
				(b1 & facemask).any() &&
				(b2 & facemask).any()) {
				triangles[TriangulationResult::IDXBUFF_FACE].push_back(i0);
				triangles[TriangulationResult::IDXBUFF_FACE].push_back(i1);
				triangles[TriangulationResult::IDXBUFF_FACE].push_back(i2);
			}
			else if ((b0 & hullmask).any() &&
				(b1 & hullmask).any() &&
				(b2 & hullmask).any()) {
				triangles[TriangulationResult::IDXBUFF_HULL].push_back(i0);
				triangles[TriangulationResult::IDXBUFF_HULL].push_back(i1);
				triangles[TriangulationResult::IDXBUFF_HULL].push_back(i2);
			}
			else /*if ((b0 & bgmask).any() ||
				(b1 & bgmask).any() ||
				(b2 & bgmask).any())*/ { 
				triangles[TriangulationResult::IDXBUFF_BACKGROUND].push_back(i0);
				triangles[TriangulationResult::IDXBUFF_BACKGROUND].push_back(i1);
				triangles[TriangulationResult::IDXBUFF_BACKGROUND].push_back(i2);
			}
		}

		// Build index buffers
		// TODO: again, best to leave graphics calls to render() method
		//
		for (int i = 0; i < TriangulationResult::NUM_INDEX_BUFFERS; i++) {
			if (i == TriangulationResult::IDXBUFF_LINES && !result.buildLines)
				continue;
			obs_enter_graphics();
			uint32_t* indices = (uint32_t*)bmalloc(sizeof(uint32_t) * triangles[i].size());
			memcpy(indices, triangles[i].data(), sizeof(uint32_t) * triangles[i].size());
			result.indexBuffers[i] = gs_indexbuffer_create(gs_index_type::GS_UNSIGNED_LONG,
				(void*)indices, triangles[i].size(), 0);
			obs_leave_graphics();
		}
	}

	// Subdivide : insert points half-way between all the points
	//
	void OBSFaceDetector::Subdivide(std::vector<cv::Point2f>& points) {
		points.reserve(points.size() * 2);
		for (unsigned int i = 0; i < points.size(); i++) {
			int i2 = (i + 1) % points.size();
			points.insert(points.begin()+i2, cv::Point2f(
				(points[i].x + points[i2].x) / 2.0f,
				(points[i].y + points[i2].y) / 2.0f));
			i++;
		}
	}

	// Curve Fitting - Catmull-Rom spline
	// https://gist.github.com/pr0digy/1383576
	// - converted to C++
	// - modified for my uses
	void OBSFaceDetector::CatmullRomSmooth(std::vector<cv::Point2f>& points, 
		const std::vector<int>& indices, int steps) {

		if (indices.size() < 3)
			return;

		points.reserve(points.size() + ((indices.size() - 1) * (steps - 1)));

		float dt = 1.0f / (float)steps;

		float x, y;
		size_t i0, i1, i2, i3;
		size_t count = indices.size() - 1;
		size_t count_m1 = count - 1;
		for (size_t i = 0; i < count; i++) {
			if (i == 0) {
				// 0 0 1 2 (i == 0)
				i0 = indices[i];
				i1 = indices[i];
				i2 = indices[i + 1];
				i3 = indices[i + 2];
			}
			else if (i == count_m1) {
				// 6 7 8 8 (i == 7)
				i0 = indices[i - 1];
				i1 = indices[i];
				i2 = indices[i + 1];
				i3 = indices[i + 1];
			}
			else {
				// 2 3 4 5 (i == 3)
				i0 = indices[i - 1];
				i1 = indices[i];
				i2 = indices[i + 1];
				i3 = indices[i + 2];
			}
			const cv::Point2f& p0 = points[i0];
			const cv::Point2f& p1 = points[i1];
			const cv::Point2f& p2 = points[i2];
			const cv::Point2f& p3 = points[i3];

			// TODO: we have more than a couple of SSE-enabled math libraries on tap
			//       we should be using one here
			//       ie) dlib/openCV/libOBS
			//
			// Note: skip points at t=0 and t=1, they are already in our set
			//
			for (float t = dt; t < 1.0f; t += dt) {
				float t2 = t * t;
				float t3 = t2 * t;

				x = 0.5f * 
					((2.0f * p1.x) +
					 (p2.x - p0.x) * t +
					 (2.0f * p0.x - 5.0f * p1.x + 4.0f * p2.x - p3.x) * t2 +
					 (3.0f * p1.x - p0.x - 3.0f * p2.x + p3.x) * t3);
					
				y = 0.5f *
					((2.0f * p1.y) +
					 (p2.y - p0.y) * t +
					 (2.0f * p0.y - 5.0f * p1.y + 4.0f * p2.y - p3.y) * t2 +
					 (3.0f * p1.y - p0.y - 3.0f * p2.y + p3.y) * t3);

				points.push_back(cv::Point2f(x, y));
			}
		}
	}

	void OBSFaceDetector::ScaleMorph(std::vector<cv::Point2f>& points,
		std::vector<int> indices, cv::Point2f& center, cv::Point2f& scale) {
		for (auto i : indices) {
			points[i].x = (points[i].x - center.x) * scale.x + center.x;
			points[i].y = (points[i].y - center.y) * scale.y + center.y;
		}
	}

	OBSFaceDetector::CropInfo OBSFaceDetector::GetCropInfo() {
		// get cropping info from config and detect image dimensions
		int ww = (int)((float)m_detect.w *
			Config::singleton().get_double(
				CONFIG_DOUBLE_FACE_DETECT_CROP_WIDTH));
		int hh = (int)((float)m_detect.h *
			Config::singleton().get_double(
				CONFIG_DOUBLE_FACE_DETECT_CROP_HEIGHT));
		int xx = (int)((float)(m_detect.w / 2) *
			Config::singleton().get_double(CONFIG_DOUBLE_FACE_DETECT_CROP_X)) +
			(m_detect.w / 2);
		int yy = (int)((float)(m_detect.h / 2) *
			Config::singleton().get_double(CONFIG_DOUBLE_FACE_DETECT_CROP_Y)) +
			(m_detect.h / 2);

		CropInfo cropInfo(xx, yy, ww, hh);
		return cropInfo;
	}

    void OBSFaceDetector::DoFaceDetection() {

		// get cropping info from config and detect image dimensions
		CropInfo cropInfo = GetCropInfo();
		// need to scale back
		float scale = (float)m_capture.width / m_detect.w;

        // Detect faces using FaceLib::FaceDetector::DetectFaces
		std::vector<rectangle> faces;
		_faceDetector.DetectFaces(currentImage, faces);

		// only consider the face detection results if:
        //
        // - tracking is disabled (so we have to)
        // - we currently have no faces
        // - face detection just found some faces
        //
        // otherwise, we are tracking faces, and the tracking is still trusted, so don't trust
        // the FD results
        //
        if ((m_faces.length == 0) || (faces.size() > 0)) {
            // clamp to max faces
			m_faces.length = (int)faces.size() > MAX_FACES ? MAX_FACES : (int)faces.size();

            // copy rects into our faces, start tracking
            for (int i = 0; i < m_faces.length; i++) {
                // scale rectangle up to video frame size
				m_faces[i].m_bounds.set_left((long)((float)(faces[i].left() +
					cropInfo.offsetX) * scale));
                m_faces[i].m_bounds.set_right((long)((float)(faces[i].right() +
					cropInfo.offsetX) * scale));
                m_faces[i].m_bounds.set_top((long)((float)(faces[i].top() +
					cropInfo.offsetY) * scale));
                m_faces[i].m_bounds.set_bottom((long)((float)(faces[i].bottom() +
					cropInfo.offsetY) * scale));
            }
        }
    }
    
        
    void OBSFaceDetector::StartObjectTracking() {

		// TODO: Complete full OpenCV 4.0.0 integration
		//		Debug and Release and necessary dll's
		//		Change the Face Data Structure
		//		Log everything and then peace!
		// get crop info from config and track image dimensions
		CropInfo cropInfo = GetCropInfo();

		// need to scale back
		float scale = (float)m_capture.width / m_detect.w;

        // start tracking
		dlib::cv_image<unsigned char> img(currentImage);
		for (int i = 0; i < m_faces.length; ++i) {
			m_faces[i].StartTracking(img, scale, cropInfo.offsetX, cropInfo.offsetY);
		}
	}
    
    
    void OBSFaceDetector::UpdateObjectTracking() {

		// get crop info from config and track image dimensions
		CropInfo cropInfo = GetCropInfo();

		char* cropdata = m_detect.data +
			(m_detect.getStride() * cropInfo.offsetY) +
			(m_detect.getNumElems() * cropInfo.offsetX);

		// update object tracking
		dlib::cv_image<unsigned char> img(currentImage);
		for (int i = 0; i < m_faces.length; i++) {
			if (i == m_trackingFaceIndex) {
				double confidence = m_faces[i].UpdateTracking(img);
				if (confidence < Config::singleton().get_double(
					CONFIG_DOUBLE_TRACKING_THRESHOLD)) {
					m_faces.length = 0;
					break;
				}
			}
		}
	}
    
    
    void OBSFaceDetector::DetectLandmarks(const OBSTexture& capture, DetectionResults& results)
    {
		// convenience
		m_capture = capture;

		// detect landmarks
		obs_enter_graphics();
		StageCaptureTexture();
		for (int f = 0; f < m_faces.length; f++) {

			// Detect features on full-size frame
			//full_object_detection d68;
			cv::Mat gray;
			switch (m_stageWork.type) {
			case IMAGETYPE_BGR:
			{
				cv::Mat bgrImage(m_stageWork.h, m_stageWork.w, CV_8UC3, m_stageWork.data, m_stageWork.getStride());
				cv::cvtColor(bgrImage, gray, cv::COLOR_BGR2GRAY);
				break;
			}
			case IMAGETYPE_RGB:
			{
				cv::Mat rgbImage(m_stageWork.h, m_stageWork.w, CV_8UC3, m_stageWork.data, m_stageWork.getStride());
				cv::cvtColor(rgbImage, gray, cv::COLOR_RGB2GRAY);
				break;
			}
			case IMAGETYPE_RGBA:
			{
				cv::Mat rgbaImage(m_stageWork.h, m_stageWork.w, CV_8UC4, m_stageWork.data, m_stageWork.getStride());
				cv::cvtColor(rgbaImage, gray, cv::COLOR_RGBA2GRAY);
				break;
			}
			case IMAGETYPE_GRAY:
			{
				gray = cv::Mat(m_stageWork.h, m_stageWork.w, CV_8UC1, m_stageWork.data, m_stageWork.getStride());
				break;
			}
			default:
				throw std::invalid_argument(
					"bad image type for face detection - handle better");
				break;
			}

			// Predict Landmarks
			std::vector<dlib::point> landmarks;
			_faceLandmarks.DetectLandmarks(gray, m_faces[f].m_bounds, landmarks);
			for (int j = 0; j < NUM_FACIAL_LANDMARKS; j++) {
				results[f].landmarks68[j] = landmarks[j];
			}
		}
		UnstageCaptureTexture();
		obs_leave_graphics();
		results.length = m_faces.length;
	}

	float OBSFaceDetector::ReprojectionError(const std::vector<cv::Point3f>& model_points,
		const std::vector<cv::Point2f>& image_points,
		const cv::Mat& rotation, const cv::Mat& translation) {

		// reproject to check error
		std::vector<cv::Point2f> projectedPoints;
		cv::projectPoints(model_points, rotation, translation,
			GetCVCamMatrix(), GetCVDistCoeffs(), projectedPoints);
		// Compute error
		std::vector<cv::Point2f> absDiff;
		cv::absdiff(image_points, projectedPoints, absDiff);
		float error = cv::sum(cv::mean(absDiff))[0];
		return error;
	}


	void OBSFaceDetector::DoPoseEstimation(DetectionResults& results)
	{
		// Build a set of model points to use for solving 3D pose
		// NOTE: The keypoints sould be selected w.r.t stability
		// TODO: Reduce Keypoints, change pose solver,
		//       change per-frame pose to temporal pose
		std::vector<int> model_indices;
		model_indices.push_back(LEFT_OUTER_EYE_CORNER);
		model_indices.push_back(RIGHT_OUTER_EYE_CORNER);
		model_indices.push_back(NOSE_1);
		model_indices.push_back(NOSE_2);
		model_indices.push_back(NOSE_3);
		model_indices.push_back(NOSE_4);
		model_indices.push_back(NOSE_7);
		std::vector<cv::Point3f> model_points = GetLandmarkPoints(model_indices);

		if (m_poses.length != results.length) {
			m_poses.length = results.length;
			for (int i = 0; i < m_poses.length; i++) {
				m_poses[i].ResetPose();
			}
		}

		float threshold = 4.0f * model_points.size();
		threshold *= ((float)CaptureWidth() / 1920.0f);

		bool resultsBad = false;
		for (int i = 0; i < results.length; i++) {
			std::vector<cv::Point2f> image_points;
			// copy 2D image points. 
			point* p = results[i].landmarks68;
			for (int j = 0; j < model_indices.size(); j++) {
				int idx = model_indices[j];
				image_points.push_back(cv::Point2f((float)p[idx].x(), (float)p[idx].y()));
			}

			// Solve for pose
			cv::Mat translation = m_poses[i].GetCVTranslation();
			cv::Mat rotation = m_poses[i].GetCVRotation();
			cv::solvePnP(model_points, image_points,
				GetCVCamMatrix(), GetCVDistCoeffs(),
				rotation, translation,
				m_poses[i].PoseValid());

			// sometimes we get crap
			if (translation.at<double>(2, 0) > 1000.0 ||
				translation.at<double>(2, 0) < -1000.0) {
				resultsBad = true;
				m_poses.length = 0;
				break;
			}

			// check error again, still bad, use previous pose
			if (m_poses[i].PoseValid() &&
				ReprojectionError(model_points, image_points, rotation, translation) > threshold) {
				// reset pose
				translation = m_poses[i].GetCVTranslation();
				rotation = m_poses[i].GetCVRotation();
			}

			// Save it
			m_poses[i].SetPose(rotation, translation);
			results[i].SetPose(m_poses[i]);
		}

		if (resultsBad) {
			// discard results
			results.length = 0;
		}
	}

	void OBSFaceDetector::StageCaptureTexture() {
		// need to stage the surface so we can read from it
		// (re)alloc the stage surface if necessary
		if (m_captureStage == nullptr ||
			(int)gs_stagesurface_get_width(m_captureStage) != m_capture.width ||
			(int)gs_stagesurface_get_height(m_captureStage) != m_capture.height) {
			if (m_captureStage)
				gs_stagesurface_destroy(m_captureStage);
			m_captureStage = gs_stagesurface_create(m_capture.width, m_capture.height,
				gs_texture_get_color_format(m_capture.texture));
		}
		gs_stage_texture(m_captureStage, m_capture.texture);

		// mapping the stage surface 
		uint8_t *data; uint32_t linesize;
		if (gs_stagesurface_map(m_captureStage, &data, &linesize)) {

			// Wrap the staged texture data
			m_stageWork.w = m_capture.width;
			m_stageWork.h = m_capture.height;
			m_stageWork.stride = linesize;
			m_stageWork.type = OBSRenderer::OBSToSMLL(
				gs_texture_get_color_format(m_capture.texture));
			m_stageWork.data = (char*)data;
		}
		else {
			blog(LOG_DEBUG, "unable to stage texture!!! bad news!");
			m_stageWork = ImageWrapper();
		}
	}

	void OBSFaceDetector::UnstageCaptureTexture() {
		// unstage the surface and leave graphics context
		gs_stagesurface_unmap(m_captureStage);
	}

	void OBSFaceDetector::ResetFaces() {
		m_faces.length = 0;
		m_detectionTimeout = 0;
	}
	
} // smll namespace




