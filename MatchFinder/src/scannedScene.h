
#pragma once

#include "mLibInclude.h"

#include "keyPoint.h"



inline float gaussR(float sigma, float dist)
{
	return exp(-(dist*dist) / (2.0f*sigma*sigma));
}
inline float gaussR(float sigma, const vec3f& d)
{
	float dist = d.length();
	return exp(-(dist*dist) / (2.0f*sigma*sigma));
}
inline float gaussR(float sigma, const vec3uc& d)
{
	vec3f _d(d);	//_d /= 255.0f;
	float dist = _d.length();
	return exp(-(dist*dist) / (2.0f*sigma*sigma));
}


inline float linearR(float sigma, float dist)
{
	return std::max(1.0f, std::min(0.0f, 1.0f - (dist*dist) / (2.0f*sigma*sigma)));
}

inline float gaussD(float sigma, int x, int y)
{
	return exp(-((x*x + y*y) / (2.0f*sigma*sigma)));
}

inline float gaussD(float sigma, int x)
{
	return exp(-((x*x) / (2.0f*sigma*sigma)));
}

void bilateralFilter(BaseImage<vec3uc>& img, float sigmaD, float sigmaR) {

	BaseImage<vec3uc> res(img.getDimensions());
	res.setInvalidValue(img.getInvalidValue());

	const int kernelRadius = (int)ceil(2.0*sigmaD);
	for (unsigned int y = 0; y < img.getHeight(); y++) {
		for (unsigned int x = 0; x < img.getWidth(); x++) {

			res.setInvalid(x, y);

			vec3f sum = vec3f(0.0f);
			float sumWeight = 0.0f;

			if (img.isValid(x, y)) {
				const vec3uc& center = img(x, y);

				for (int m = x - kernelRadius; m <= (int)x + kernelRadius; m++) {
					for (int n = y - kernelRadius; n <= (int)y + kernelRadius; n++) {
						if (m >= 0 && n >= 0 && m < (int)img.getWidth() && n < (int)img.getHeight()) {
							if (img.isValid(m, n)) {
								const vec3uc& current = img(m, n);
								const float weight = gaussD(sigmaD, m - x, n - y)*gaussR(sigmaR, current - center);
								sumWeight += weight;
								sum += weight*vec3f(current);
							}
						}
					}
				}
				if (sumWeight > 0.0f) res(x, y) = math::round(sum / sumWeight);
			}
		}
	}
	img = res;
}

class ScannedScene {
public:
	ScannedScene(const std::string& path, const std::string& name) {
		load(path, name);
	}
	~ScannedScene() {
		for (auto* sd : m_sds) {
			SAFE_DELETE(sd);
		}
	}

	void load(const std::string& path, const std::string& name) {

		m_name = name;

		Directory dir(path);
		auto& files = dir.getFilesWithSuffix(".sens");
		std::sort(files.begin(), files.end());

		for (auto& f : files) {
			m_sds.push_back(new SensorData);
			SensorData* sd = m_sds.back();
			sd->loadFromFile(path + "/" + f);
			std::cout << *sd << std::endl;

			//break;
		}
	}

	void findKeyPoints() {
		for (size_t sensorIdx = 0; sensorIdx < m_sds.size(); sensorIdx++) {
			SensorData* sd = m_sds[sensorIdx];
			const mat4f intrinsicInv = sd->m_calibrationDepth.m_intrinsic.getInverse();

			for (size_t imageIdx = 0; imageIdx < sd->m_frames.size(); imageIdx++) {
				ColorImageR8G8B8 c = sd->computeColorImage(imageIdx);
				DepthImage32 d = sd->computeDepthImage(imageIdx);

				//float sigmaD = 2.0f;	
				//float sigmaR = 0.1f;
				//sigmaD = 10.0f;
				//sigmaR = 10.0f;
				//FreeImageWrapper::saveImage("_before.png", c);
				//bilateralFilter(c, sigmaD, sigmaR);
				//FreeImageWrapper::saveImage("_after.png", c);
				//std::cout << "here" << std::endl;
				//getchar();

				const mat4f& camToWorld = sd->m_frames[imageIdx].getCameraToWorld();

				const unsigned int maxNumKeyPoints = 512;
				const float minResponse = 0.03f;
				std::vector<vec4f> rawKeyPoints = KeyPointFinder::findKeyPoints(c, maxNumKeyPoints, minResponse);

				MeshDataf md;
				size_t validKeyPoints = 0;
				for (vec4f& rawkp : rawKeyPoints) {
					const unsigned int padding = 50;	//don't take keypoints in the padding region of the image
					vec2ui loc = math::round(vec2f(rawkp.x, rawkp.y));
					if (d.isValid(loc) && d.isValidCoordinate(loc + padding) && d.isValidCoordinate(loc - padding)) {
						KeyPoint kp;
						kp.m_depth = d(loc);
						kp.m_frameIdx = (unsigned int)imageIdx;
						kp.m_sensorIdx = (unsigned int)sensorIdx;
						//kp.m_pixelPos = vec2f(rawkp.x, rawkp.y);
						kp.m_pixelPos = vec2f(loc);
						kp.m_size = rawkp.z;
						kp.m_response = rawkp.w;

						vec3f cameraPos = (intrinsicInv*vec4f(kp.m_pixelPos.x*kp.m_depth, kp.m_pixelPos.y*kp.m_depth, kp.m_depth, 0.0f)).getVec3();
						kp.m_worldPos = camToWorld * cameraPos;

						validKeyPoints++;

						m_keyPoints.push_back(kp);

						if (imageIdx == 0) md.merge(Shapesf::sphere(0.01f, vec3f(kp.m_worldPos), 10, 10, vec4f(1.0f, 0.0f, 0.0f, 1.0f)).computeMeshData());
					}
				}

				std::cout << "\tfound " << validKeyPoints << " keypoints for image " << sensorIdx << "|" << imageIdx << std::endl;
				//if (imageIdx == 0) MeshIOf::saveToFile("test.ply", md);
				//if (imageIdx == 50) break;
			}
		}
	}


	void matchKeyPoints() {
		const float radius = 0.02f;	//10 cm
		const unsigned int maxK = 5;

		unsigned int currKeyPoint = 0;
		std::vector<std::vector<NearestNeighborSearchFLANNf*>> nns(m_sds.size());
		std::vector<std::vector<unsigned int>> nn_offsets(m_sds.size());
		for (size_t sensorIdx = 0; sensorIdx < nns.size(); sensorIdx++) {
			nns[sensorIdx].resize(m_sds[sensorIdx]->m_frames.size(), nullptr);
			nn_offsets[sensorIdx].resize(m_sds[sensorIdx]->m_frames.size(), 0);
			for (size_t frameIdx = 0; frameIdx < m_sds[sensorIdx]->m_frames.size(); frameIdx++) {

				nn_offsets[sensorIdx][frameIdx] = currKeyPoint;

				std::vector<float> points;
				for (;	currKeyPoint < (UINT)m_keyPoints.size() &&
					m_keyPoints[currKeyPoint].m_sensorIdx == sensorIdx &&
					m_keyPoints[currKeyPoint].m_frameIdx == frameIdx; currKeyPoint++) {
					points.push_back(m_keyPoints[currKeyPoint].m_worldPos.x);
					points.push_back(m_keyPoints[currKeyPoint].m_worldPos.y);
					points.push_back(m_keyPoints[currKeyPoint].m_worldPos.z);
				}

				if (points.size())	{
					nns[sensorIdx][frameIdx] = new NearestNeighborSearchFLANNf(4 * maxK, 1);
					nns[sensorIdx][frameIdx]->init(points.data(), (unsigned int)points.size() / 3, 3, maxK);
				}
			}
		}


		for (size_t keyPointIdx = 0; keyPointIdx < m_keyPoints.size(); keyPointIdx++) {
			KeyPoint& kp = m_keyPoints[keyPointIdx];
			const float* query = (const float*)&kp.m_worldPos;

			const size_t sensorIdx = kp.m_sensorIdx;
			const size_t frameIdx = kp.m_frameIdx;

			for (size_t sensorIdx_dst = sensorIdx; sensorIdx_dst < nns.size(); sensorIdx_dst++) {

				size_t frameIdx_dst = 0;
				if (sensorIdx_dst == sensorIdx) frameIdx_dst = frameIdx + 1;

				for (; frameIdx_dst < nns[sensorIdx].size(); frameIdx_dst++) {

					auto* nn = nns[sensorIdx_dst][frameIdx_dst];
					if (nn == nullptr) continue;

					size_t numMatches = 0;
					std::vector<unsigned int> res = nn->fixedRadius(query, maxK, radius);
					auto resPair = nn->fixedRadiusDist(query, maxK, radius);
					auto resDist = nn->getDistances((UINT)res.size());
					for (size_t j = 0; j < res.size(); j++) {
						KeyPointMatch m;
						m.m_kp0 = m_keyPoints[keyPointIdx];
						m.m_kp1 = m_keyPoints[res[j] + nn_offsets[sensorIdx_dst][frameIdx_dst]];

						mat4f worldToCam_kp1 = m_sds[m.m_kp1.m_sensorIdx]->m_frames[m.m_kp1.m_frameIdx].getCameraToWorld().getInverse();
						vec3f p = worldToCam_kp1 * m.m_kp0.m_worldPos;
						p = m_sds[m.m_kp1.m_sensorIdx]->m_calibrationDepth.cameraToProj(p);
						m.m_offset = m.m_kp1.m_pixelPos - vec2f(p.x, p.y);
						m_keyPointMatches.push_back(m);
						numMatches++;

						//std::cout << "orig: " << m.m_kp1.m_pixelPos << std::endl;
						//std::cout << "repr: " << vec2f(p.x, p.y) << std::endl;
						//std::cout << m.m_offset << std::endl;

						//std::cout << "match between: " << std::endl;
						//std::cout << m.m_kp0;
						//std::cout << m.m_kp1;

						//std::cout << "dist " << sensorIdx << ":\t" << (m.m_kp0.m_worldPos - m.m_kp1.m_worldPos).length() << std::endl;
						//std::cout << std::endl;
						//int a = 5;
					}
				}
			}
		}


		//clean up our mess...
		for (size_t sensorIdx = 0; sensorIdx < nns.size(); sensorIdx++) {
			for (size_t frameIdx = 0; frameIdx < nns[sensorIdx].size(); frameIdx++) {
				SAFE_DELETE(nns[sensorIdx][frameIdx]);
			}
		}


		std::cout << "TOTAL MATCHES FOUND " << m_keyPointMatches.size() << std::endl;
	}

	void saveMatches(const std::string& filename) {
		std::ofstream outFile(filename);

		outFile << "SceneName " << m_name << "\n";
		for (size_t i = 0; i < m_keyPointMatches.size(); i++) {
			outFile << m_keyPointMatches[i] << "\n";
		}
		
	}
private:
	std::vector<SensorData*> m_sds;
	std::string m_name;

	std::vector<KeyPoint>		m_keyPoints;
	std::vector<KeyPointMatch>	m_keyPointMatches;

};