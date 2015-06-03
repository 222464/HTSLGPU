#include "HTSL.h"

using namespace htsl;

void HTSL::Kernels::loadFromProgram(sys::ComputeProgram &program) {
	// Create kernels
	_predictionInitializeKernel = cl::Kernel(program.getProgram(), "htsl_predictionInitialize");

	_predictKernel = cl::Kernel(program.getProgram(), "htsl_predict");

	_predictionLearnKernel = cl::Kernel(program.getProgram(), "htsl_predictionLearn");
}

void HTSL::createRandom(const std::vector<RecurrentSparseCoder2D::Configuration> &rscConfigs,
	int predictionRadiusFromE, int predictionRadiusFromI,
	float minInitEWeight, float maxInitEWeight,
	float minInitIWeight, float maxInitIWeight,
	float initEThreshold, float initIThreshold,
	sys::ComputeSystem &cs, const std::shared_ptr<RecurrentSparseCoder2D::Kernels> &rscKernels,
	const std::shared_ptr<Kernels> &htslKernels, std::mt19937 &generator)
{
	_kernels = htslKernels;
	_predictionRadiusFromE = predictionRadiusFromE;
	_predictionRadiusFromI = predictionRadiusFromI;

	_rscLayers.resize(rscConfigs.size());

	// Initialize all layers
	for (int li = 0; li < _rscLayers.size(); li++) {
		_rscLayers[li].createRandom(rscConfigs[li],
			minInitEWeight, maxInitEWeight, minInitIWeight, maxInitIWeight,
			initEThreshold, initIThreshold,
			cs, rscKernels, generator);
	}

	int predictionFromESize = std::pow(_predictionRadiusFromE * 2 + 1, 2);
	int predictionFromISize = std::pow(_predictionRadiusFromI * 2 + 1, 2);

	_prediction = cl::Image2D(cs.getContext(), CL_MEM_READ_WRITE, cl::ImageFormat(CL_R, CL_FLOAT), rscConfigs.front()._eFeedForwardWidth, rscConfigs.front()._eFeedForwardHeight);

	_predictionFromEWeights._weights = cl::Image3D(cs.getContext(), CL_MEM_READ_WRITE, cl::ImageFormat(CL_R, CL_FLOAT), rscConfigs.front()._eFeedForwardWidth, rscConfigs.front()._eFeedForwardHeight, predictionFromESize);
	_predictionFromEWeights._weightsPrev = cl::Image3D(cs.getContext(), CL_MEM_READ_WRITE, cl::ImageFormat(CL_R, CL_FLOAT), rscConfigs.front()._eFeedForwardWidth, rscConfigs.front()._eFeedForwardHeight, predictionFromESize);
	
	_predictionFromIWeights._weights = cl::Image3D(cs.getContext(), CL_MEM_READ_WRITE, cl::ImageFormat(CL_R, CL_FLOAT), rscConfigs.front()._eFeedForwardWidth, rscConfigs.front()._eFeedForwardHeight, predictionFromISize);
	_predictionFromIWeights._weightsPrev = cl::Image3D(cs.getContext(), CL_MEM_READ_WRITE, cl::ImageFormat(CL_R, CL_FLOAT), rscConfigs.front()._eFeedForwardWidth, rscConfigs.front()._eFeedForwardHeight, predictionFromISize);

	std::uniform_int_distribution<int> seedDist(0, 10000);

	cl_uint2 seed = { seedDist(generator), seedDist(generator) };

	int index = 0;

	_kernels->_predictionInitializeKernel.setArg(index++, _predictionFromEWeights._weightsPrev);
	_kernels->_predictionInitializeKernel.setArg(index++, _predictionFromIWeights._weightsPrev);
	_kernels->_predictionInitializeKernel.setArg(index++, predictionFromESize);
	_kernels->_predictionInitializeKernel.setArg(index++, predictionFromISize);
	_kernels->_predictionInitializeKernel.setArg(index++, minInitEWeight);
	_kernels->_predictionInitializeKernel.setArg(index++, maxInitEWeight);
	_kernels->_predictionInitializeKernel.setArg(index++, seed);

	cs.getQueue().enqueueNDRangeKernel(_kernels->_predictionInitializeKernel, cl::NullRange, cl::NDRange(rscConfigs.front()._eFeedForwardWidth, rscConfigs.front()._eFeedForwardHeight));
}

void HTSL::update(sys::ComputeSystem &cs, const cl::Image2D &inputImage, const cl::Image2D &zeroImage, float eta, float homeoDecay) {
	const cl::Image2D* pLayerInput = &inputImage;

	// Feed forward
	for (int li = 0; li < _rscLayers.size(); li++) {
		_rscLayers[li].eActivate(cs, *pLayerInput, eta, homeoDecay);

		pLayerInput = &_rscLayers[li]._eLayer._states;
	}

	pLayerInput = &zeroImage;

	// Feed back
	for (int li = _rscLayers.size() - 1; li >= 0; li--) {
		_rscLayers[li].iActivate(cs, *pLayerInput, eta, homeoDecay);

		pLayerInput = &_rscLayers[li]._iLayer._states;
	}
}

void HTSL::predict(sys::ComputeSystem &cs) {
	cl_float2 eFeedForwardDimsToEDims = { static_cast<float>(_rscLayers.front().getConfig()._eWidth + 1) / static_cast<float>(_rscLayers.front().getConfig()._eFeedForwardWidth + 1), static_cast<float>(_rscLayers.front().getConfig()._eHeight + 1) / static_cast<float>(_rscLayers.front().getConfig()._eFeedForwardHeight + 1) };
	cl_float2 eFeedForwardDimsToIDims = { static_cast<float>(_rscLayers.front().getConfig()._iWidth + 1) / static_cast<float>(_rscLayers.front().getConfig()._eFeedForwardWidth + 1), static_cast<float>(_rscLayers.front().getConfig()._iHeight + 1) / static_cast<float>(_rscLayers.front().getConfig()._eFeedForwardHeight + 1) };

	cl_int2 eDims = { _rscLayers.front().getConfig()._eWidth, _rscLayers.front().getConfig()._eHeight };
	cl_int2 iDims = { _rscLayers.front().getConfig()._iWidth, _rscLayers.front().getConfig()._iHeight };

	int index = 0;

	_kernels->_predictKernel.setArg(index++, _rscLayers.front()._eLayer._states);
	_kernels->_predictKernel.setArg(index++, _rscLayers.front()._iLayer._states);
	_kernels->_predictKernel.setArg(index++, _predictionFromEWeights._weightsPrev);
	_kernels->_predictKernel.setArg(index++, _predictionFromIWeights._weightsPrev);
	_kernels->_predictKernel.setArg(index++, _prediction);
	
	_kernels->_predictKernel.setArg(index++, eFeedForwardDimsToEDims);
	_kernels->_predictKernel.setArg(index++, eFeedForwardDimsToIDims);
	_kernels->_predictKernel.setArg(index++, eDims);
	_kernels->_predictKernel.setArg(index++, iDims);
	_kernels->_predictKernel.setArg(index++, _predictionRadiusFromE);
	_kernels->_predictKernel.setArg(index++, _predictionRadiusFromI);

	cs.getQueue().enqueueNDRangeKernel(_kernels->_predictKernel, cl::NullRange, cl::NDRange(_rscLayers.front().getConfig()._eFeedForwardWidth, _rscLayers.front().getConfig()._eFeedForwardHeight));
}

void HTSL::learn(sys::ComputeSystem &cs, const cl::Image2D &inputImage, const cl::Image2D &zeroImage,
	float eAlpha, float eBeta, float eDelta, float iAlpha, float iBeta, float iGamma, float iDelta,
	float sparsity)
{
	for (int li = 0; li < _rscLayers.size(); li++) {
		if (li == 0) {
			if (li == _rscLayers.size() - 1)
				_rscLayers[li].learn(cs, inputImage, zeroImage, eAlpha, eBeta, eDelta, iAlpha, iBeta, iGamma, iDelta, sparsity);
			else
				_rscLayers[li].learn(cs, inputImage, _rscLayers[li + 1]._iLayer._states, eAlpha, eBeta, eDelta, iAlpha, iBeta, iGamma, iDelta, sparsity);
		}
		else {
			if (li == _rscLayers.size() - 1)
				_rscLayers[li].learn(cs, _rscLayers[li - 1]._eLayer._states, zeroImage, eAlpha, eBeta, eDelta, iAlpha, iBeta, iGamma, iDelta, sparsity);
			else
				_rscLayers[li].learn(cs, _rscLayers[li - 1]._eLayer._states, _rscLayers[li + 1]._iLayer._states, eAlpha, eBeta, eDelta, iAlpha, iBeta, iGamma, iDelta, sparsity);
		}
	}
}

void HTSL::learnPrediction(sys::ComputeSystem &cs, const cl::Image2D &inputImage, float alpha) {
	cl_float2 eFeedForwardDimsToEDims = { static_cast<float>(_rscLayers.front().getConfig()._eWidth + 1) / static_cast<float>(_rscLayers.front().getConfig()._eFeedForwardWidth + 1), static_cast<float>(_rscLayers.front().getConfig()._eHeight + 1) / static_cast<float>(_rscLayers.front().getConfig()._eFeedForwardHeight + 1) };
	cl_float2 eFeedForwardDimsToIDims = { static_cast<float>(_rscLayers.front().getConfig()._iWidth + 1) / static_cast<float>(_rscLayers.front().getConfig()._eFeedForwardWidth + 1), static_cast<float>(_rscLayers.front().getConfig()._iHeight + 1) / static_cast<float>(_rscLayers.front().getConfig()._eFeedForwardHeight + 1) };

	cl_int2 eDims = { _rscLayers.front().getConfig()._eWidth, _rscLayers.front().getConfig()._eHeight };
	cl_int2 iDims = { _rscLayers.front().getConfig()._iWidth, _rscLayers.front().getConfig()._iHeight };

	int index = 0;

	_kernels->_predictionLearnKernel.setArg(index++, _rscLayers.front()._eLayer._states);
	_kernels->_predictionLearnKernel.setArg(index++, _rscLayers.front()._iLayer._states);
	_kernels->_predictionLearnKernel.setArg(index++, inputImage);
	_kernels->_predictionLearnKernel.setArg(index++, _prediction);
	_kernels->_predictionLearnKernel.setArg(index++, _predictionFromEWeights._weightsPrev);
	_kernels->_predictionLearnKernel.setArg(index++, _predictionFromIWeights._weightsPrev);
	_kernels->_predictionLearnKernel.setArg(index++, _predictionFromEWeights._weights);
	_kernels->_predictionLearnKernel.setArg(index++, _predictionFromIWeights._weights);

	_kernels->_predictionLearnKernel.setArg(index++, eFeedForwardDimsToEDims);
	_kernels->_predictionLearnKernel.setArg(index++, eFeedForwardDimsToIDims);
	_kernels->_predictionLearnKernel.setArg(index++, eDims);
	_kernels->_predictionLearnKernel.setArg(index++, iDims);
	_kernels->_predictionLearnKernel.setArg(index++, _predictionRadiusFromE);
	_kernels->_predictionLearnKernel.setArg(index++, _predictionRadiusFromI);
	_kernels->_predictionLearnKernel.setArg(index++, alpha);

	cs.getQueue().enqueueNDRangeKernel(_kernels->_predictionLearnKernel, cl::NullRange, cl::NDRange(_rscLayers.front().getConfig()._eFeedForwardWidth, _rscLayers.front().getConfig()._eFeedForwardHeight));
}

void HTSL::stepEnd() {
	for (int li = 0; li < _rscLayers.size(); li++)
		_rscLayers[li].stepEnd();

	std::swap(_predictionFromEWeights._weights, _predictionFromEWeights._weightsPrev);
	std::swap(_predictionFromIWeights._weights, _predictionFromIWeights._weightsPrev);
}

void htsl::generateConfigsFromSizes(cl_int2 inputSize, const std::vector<cl_int2> &layerESizes, const std::vector<cl_int2> &layerISizes, std::vector<RecurrentSparseCoder2D::Configuration> &configs) {
	assert(layerESizes.size() == layerISizes.size());
	
	if (configs.size() != layerESizes.size())
		configs.resize(layerESizes.size());

	for (int li = 0; li < configs.size(); li++) {
		if (li == 0) {
			configs[li]._eFeedForwardWidth = inputSize.x;
			configs[li]._eFeedForwardHeight = inputSize.y;
		}
		else {
			configs[li]._eFeedForwardWidth = layerESizes[li - 1].x;
			configs[li]._eFeedForwardHeight = layerESizes[li - 1].y;
		}

		configs[li]._eWidth = layerESizes[li].x;
		configs[li]._eHeight = layerESizes[li].y;

		configs[li]._iWidth = layerISizes[li].x;
		configs[li]._iHeight = layerISizes[li].y;

		if (li == configs.size() - 1) {
			configs[li]._iFeedBackWidth = 1;
			configs[li]._iFeedBackHeight = 1;
		}
		else {
			configs[li]._iFeedBackWidth = layerISizes[li + 1].x;
			configs[li]._iFeedBackHeight = layerISizes[li + 1].y;
		}
	}
}