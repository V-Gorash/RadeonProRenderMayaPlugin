#include "FireRenderToonMaterial.h"
#include "FireMaya.h"
#include "FireRenderUtils.h"
#include <maya/MSelectionList.h>
#include <maya/MUuid.h>
#include <maya/MDGMessage.h>
#include <maya/MNodeMessage.h>

namespace
{
	namespace Attribute
	{
		MObject output;

		MObject showAdvanced;
		MObject showMixLevels;

		// toon closure
		MObject color;
		MObject normal;
		MObject roughness;

		MObject transparencyEnable;
		MObject transparencyLevel;

		// ramp
		MObject enable5Colors;

		MObject highlightColor;
		MObject highlightColor2;
		MObject midColor;
		MObject shadowColor;
		MObject shadowColor2;

		MObject rampPositionShadow;
		MObject rampPosition1;
		MObject rampPosition2;
		MObject rampPositionHighlight;

		MObject rampRangeShadow;
		MObject rampRange1;
		MObject rampRange2;
		MObject rampRangeHighlight;

		// light linking
		MObject enableLightLinking;
		MObject linkedLight;
	}
}

// Register attribute and make it affecting output color and alpha
#define ADD_ATTRIBUTE(attr) \
	CHECK_MSTATUS(addAttribute(attr)); \
	CHECK_MSTATUS(attributeAffects(attr, Attribute::output));


MStatus FireMaya::ToonMaterial::initialize()
{
	MFnNumericAttribute nAttr;
	MFnEnumAttribute eAttr;

	Attribute::output = nAttr.createColor("outColor", "oc");
	MAKE_OUTPUT(nAttr);

	Attribute::showAdvanced = nAttr.create("showAdvanced", "sa", MFnNumericData::kBoolean, false);
	MAKE_INPUT(nAttr);
	nAttr.setConnectable(false);

	Attribute::showMixLevels = nAttr.create("showMixLevels", "sml", MFnNumericData::kBoolean, false);
	MAKE_INPUT(nAttr);
	nAttr.setConnectable(false);

	Attribute::color = nAttr.createColor("color", "c");
	MAKE_INPUT(nAttr);
	CHECK_MSTATUS(nAttr.setDefault(0.8, 0.8, 0.8));

	Attribute::normal = nAttr.createColor("normal", "n");
	MAKE_INPUT(nAttr);
	CHECK_MSTATUS(nAttr.setDefault(1.0f, 1.0f, 1.0f));

	Attribute::roughness = nAttr.create("roughness", "r", MFnNumericData::kFloat, 1.0);
	MAKE_INPUT(nAttr);
	nAttr.setMin(0.0);
	nAttr.setMax(1.0);

	// transparency
	Attribute::transparencyLevel = nAttr.create("transparencyLevel", "trl", MFnNumericData::kFloat, 1.0);
	MAKE_INPUT(nAttr);
	nAttr.setMin(0.0);
	nAttr.setMax(1.0);

	Attribute::transparencyEnable = nAttr.create("transparencyEnable", "et", MFnNumericData::kBoolean, 0);
	MAKE_INPUT_CONST(nAttr);
	
	// RAMP	
	Attribute::enable5Colors = nAttr.create("enable5Colors", "e5c", MFnNumericData::kBoolean, 0);
	MAKE_INPUT_CONST(nAttr);

	// Colors
	Attribute::highlightColor = nAttr.createColor("highlightColor", "hc");
	MAKE_INPUT(nAttr);
	CHECK_MSTATUS(nAttr.setDefault(0.8, 0.8, 0.8));

	Attribute::highlightColor2 = nAttr.createColor("highlightColor2", "hc2");
	MAKE_INPUT(nAttr);
	CHECK_MSTATUS(nAttr.setDefault(0.8, 0.8, 0.8));

	Attribute::midColor = nAttr.createColor("midColor", "mc");
	MAKE_INPUT(nAttr);
	CHECK_MSTATUS(nAttr.setDefault(0.4, 0.4, 0.4));

	Attribute::shadowColor = nAttr.createColor("shadowColor", "sc");
	MAKE_INPUT(nAttr);
	CHECK_MSTATUS(nAttr.setDefault(0.0, 0.0, 0.0));

	Attribute::shadowColor2 = nAttr.createColor("shadowColor2", "sc2");
	MAKE_INPUT(nAttr);
	CHECK_MSTATUS(nAttr.setDefault(0.0, 0.0, 0.0));

	// Levels - positions
	// Mid Level

	Attribute::rampPositionShadow = nAttr.create("rampPositionShadow", "rps", MFnNumericData::kFloat, 0.2);
	MAKE_INPUT(nAttr);
	nAttr.setMin(0.0);
	nAttr.setMax(1.0);

	Attribute::rampPosition1 = nAttr.create("rampPosition1", "rp1", MFnNumericData::kFloat, 0.5);
	MAKE_INPUT(nAttr);
	nAttr.setMin(0.0);
	nAttr.setMax(1.0);

	// Highlight Level
	Attribute::rampPosition2 = nAttr.create("rampPosition2", "rp2", MFnNumericData::kFloat, 0.8);
	MAKE_INPUT(nAttr);
	nAttr.setMin(0.0);
	nAttr.setMax(1.0);

	Attribute::rampPositionHighlight = nAttr.create("rampPositionHighlight", "rph", MFnNumericData::kFloat, 0.9);
	MAKE_INPUT(nAttr);
	nAttr.setMin(0.0);
	nAttr.setMax(1.0);


	// Ranges
	// Mid Level Mix

	Attribute::rampRangeShadow = nAttr.create("rampRangeShadow", "rrs", MFnNumericData::kFloat, 0.1);
	MAKE_INPUT(nAttr);
	nAttr.setMin(0.0);
	nAttr.setMax(1.0);

	Attribute::rampRange1 = nAttr.create("rampRange1", "rr1", MFnNumericData::kFloat, 0.1);
	MAKE_INPUT(nAttr);
	nAttr.setMin(0.0);
	nAttr.setMax(1.0);

	// Highlight Level Mix
	Attribute::rampRange2 = nAttr.create("rampRange2", "rr2", MFnNumericData::kFloat, 0.1);
	MAKE_INPUT(nAttr);
	nAttr.setMin(0.0);
	nAttr.setMax(1.0);

	Attribute::rampRangeHighlight = nAttr.create("rampRangeHighlight", "rrh", MFnNumericData::kFloat, 0.1);
	MAKE_INPUT(nAttr);
	nAttr.setMin(0.0);
	nAttr.setMax(1.0);

	// light linking
	Attribute::enableLightLinking = nAttr.create("enableLightLinking", "ell", MFnNumericData::kBoolean, 0);
	MAKE_INPUT(nAttr);
	nAttr.setConnectable(false);

	//Attribute::linkedLight = eAttr.create("linkedLight", "ll", 0);
	//MAKE_INPUT_CONST(eAttr);


	// Adding all attributes to the node type
	addAttribute(Attribute::output);

	ADD_ATTRIBUTE(Attribute::showAdvanced);
	ADD_ATTRIBUTE(Attribute::showMixLevels);

	ADD_ATTRIBUTE(Attribute::color);
	ADD_ATTRIBUTE(Attribute::normal);
	ADD_ATTRIBUTE(Attribute::roughness);

	ADD_ATTRIBUTE(Attribute::transparencyEnable);
	ADD_ATTRIBUTE(Attribute::transparencyLevel);

	ADD_ATTRIBUTE(Attribute::enable5Colors);
	ADD_ATTRIBUTE(Attribute::highlightColor);
	ADD_ATTRIBUTE(Attribute::highlightColor2);
	ADD_ATTRIBUTE(Attribute::midColor);
	ADD_ATTRIBUTE(Attribute::shadowColor);
	ADD_ATTRIBUTE(Attribute::shadowColor2);
	
	ADD_ATTRIBUTE(Attribute::rampPositionShadow);
	ADD_ATTRIBUTE(Attribute::rampPosition1);
	ADD_ATTRIBUTE(Attribute::rampPosition2);
	ADD_ATTRIBUTE(Attribute::rampPositionHighlight);

	ADD_ATTRIBUTE(Attribute::rampRangeShadow);
	ADD_ATTRIBUTE(Attribute::rampRange1);
	ADD_ATTRIBUTE(Attribute::rampRange2);
	ADD_ATTRIBUTE(Attribute::rampRangeHighlight);

	ADD_ATTRIBUTE(Attribute::enableLightLinking);
	//ADD_ATTRIBUTE(Attribute::linkedLight);

	return MStatus::kSuccess;
}

// creates an instance of the node
void* FireMaya::ToonMaterial::creator()
{
	return new ToonMaterial;
}

MStatus FireMaya::ToonMaterial::compute(const MPlug& plug, MDataBlock& block)
{
	if ((plug == Attribute::output) || (plug.parent() == Attribute::output))
	{
		block.setClean(plug);

		return MS::kSuccess;
	}
	else
	{
		return MS::kUnknownParameter;
	}
}

frw::Shader FireMaya::ToonMaterial::GetShader(Scope& scope)
{
	MFnDependencyNode shaderNode(thisMObject());

	frw::Shader shader(scope.MaterialSystem(), frw::ShaderTypeToon);

	bool advancedMode = shaderNode.findPlug(Attribute::showAdvanced, false).asBool();
	bool mixLevels = shaderNode.findPlug(Attribute::showMixLevels, false).asBool();

	shader.SetValue(RPR_MATERIAL_INPUT_COLOR, scope.GetValue(shaderNode.findPlug(Attribute::color, false)));

	if (advancedMode)
	{
		frw::ToonRampNode toonRamp(scope.MaterialSystem());

		int use5ColorsParam = shaderNode.findPlug(Attribute::enable5Colors, false).asBool() ? 1 : 0;
		toonRamp.SetValueInt(RPR_MATERIAL_INPUT_TOON_5_COLORS, use5ColorsParam);

		toonRamp.SetValue(RPR_MATERIAL_INPUT_HIGHLIGHT2, scope.GetValue(shaderNode.findPlug(Attribute::highlightColor2, false)));
		toonRamp.SetValue(RPR_MATERIAL_INPUT_HIGHLIGHT, scope.GetValue(shaderNode.findPlug(Attribute::highlightColor, false)));
		toonRamp.SetValue(RPR_MATERIAL_INPUT_MID, scope.GetValue(shaderNode.findPlug(Attribute::midColor, false)));
		toonRamp.SetValue(RPR_MATERIAL_INPUT_SHADOW, scope.GetValue(shaderNode.findPlug(Attribute::shadowColor, false)));
		toonRamp.SetValue(RPR_MATERIAL_INPUT_SHADOW2, scope.GetValue(shaderNode.findPlug(Attribute::shadowColor2, false)));

		toonRamp.SetValue(RPR_MATERIAL_INPUT_POSITION_SHADOW, scope.GetValue(shaderNode.findPlug(Attribute::rampPositionShadow, false)));
		toonRamp.SetValue(RPR_MATERIAL_INPUT_POSITION1, scope.GetValue(shaderNode.findPlug(Attribute::rampPosition1, false)));
		toonRamp.SetValue(RPR_MATERIAL_INPUT_POSITION2, scope.GetValue(shaderNode.findPlug(Attribute::rampPosition2, false)));
		toonRamp.SetValue(RPR_MATERIAL_INPUT_POSITION_HIGHLIGHT, scope.GetValue(shaderNode.findPlug(Attribute::rampPositionHighlight, false)));

		if (mixLevels)
		{
			toonRamp.SetValueInt(RPR_MATERIAL_INPUT_INTERPOLATION, RPR_INTERPOLATION_MODE_LINEAR);

			toonRamp.SetValue(RPR_MATERIAL_INPUT_RANGE_SHADOW, scope.GetValue(shaderNode.findPlug(Attribute::rampRangeShadow, false)));
			toonRamp.SetValue(RPR_MATERIAL_INPUT_RANGE1, scope.GetValue(shaderNode.findPlug(Attribute::rampRange1, false)));
			toonRamp.SetValue(RPR_MATERIAL_INPUT_RANGE2, scope.GetValue(shaderNode.findPlug(Attribute::rampRange2, false)));
			toonRamp.SetValue(RPR_MATERIAL_INPUT_RANGE_HIGHLIGHT, scope.GetValue(shaderNode.findPlug(Attribute::rampRangeHighlight, false)));
		}
		else
		{
			toonRamp.SetValueInt(RPR_MATERIAL_INPUT_INTERPOLATION, RPR_INTERPOLATION_MODE_NONE);
		}

		shader.SetValue(RPR_MATERIAL_INPUT_DIFFUSE_RAMP, toonRamp);
	}

	shader.SetValue(RPR_MATERIAL_INPUT_ROUGHNESS, scope.GetValue(shaderNode.findPlug(Attribute::roughness, false)));

	frw::Value normalValue = scope.GetValue(shaderNode.findPlug(Attribute::normal, false));

	const int type = normalValue.GetNodeType();

	if (type == frw::ValueTypeNormalMap || type == frw::ValueTypeBumpMap)
	{
		shader.SetValue(RPR_MATERIAL_INPUT_NORMAL, normalValue);
	}

	// If transperency on
	if (shaderNode.findPlug(Attribute::transparencyEnable, false).asBool())
	{
		frw::Shader transparentShader = frw::Shader(scope.MaterialSystem(), frw::ShaderTypeTransparent);

		frw::Value transparancyLevel = scope.GetValue(shaderNode.findPlug(Attribute::transparencyLevel, false));
		return scope.MaterialSystem().ShaderBlend(shader, transparentShader, transparancyLevel);
	}

	if (shaderNode.findPlug(Attribute::enableLightLinking, false).asBool())
	{
		linkLight(scope, shader);
	}
	
	return shader;
}

void FireMaya::ToonMaterial::postConstructor()
{
	ShaderNode::postConstructor();

	nodeAddedCallback = MDGMessage::addNodeAddedCallback(onLightAdded, kDefaultNodeType, this);
	nodeRemovedCallback = MDGMessage::addNodeRemovedCallback(onLightRemoved, kDefaultNodeType, this);
	nodeRenamedCallback = MNodeMessage::addNameChangedCallback(MObject::kNullObj, onLightRenamed, this);
}

void FireMaya::ToonMaterial::linkLight(Scope& scope, frw::Shader& shader)
{
	const RenderType renderType = scope.GetIContextInfo()->GetRenderType();
	if (renderType == RenderType::Thumbnail || renderType == RenderType::Undefined)
	{
		return; // skip if render mode is swatch
	}

	MFnDependencyNode shaderNode(thisMObject());
	
	// We have to use MEL command to get enum value as string
	MString lightName = MGlobal::executeCommandStringResult("getAttr -as " + shaderNode.uniqueName() + ".linkedLight");
	MSelectionList selection;
	MObject light;
	selection.add(lightName);
	selection.getDependNode(0, light);

	if (light.isNull()) 
	{
		MGlobal::displayError("Unable to find linked light!\n");
		return;
	}

	rpr_light rprLight = scope.GetIContextInfo()->GetRprLightFromNode(light);

	if (!rprLight)
	{
		MGlobal::displayError("Unable to find linked light!\n");
		return;
	}

	shader.xSetParameterLight(RPR_MATERIAL_INPUT_LIGHT, rprLight);
}

bool checkIsLight(MObject& node)
{
	MFnDependencyNode depNode(node);
	MString type = depNode.typeName();
	return node.hasFn(MFn::kLight) || type == "RPRPhysicalLight" || type == "RPRIES" || type == "RPRIBL";
}

void FireMaya::ToonMaterial::onLightAdded(MObject& node, void* clientData)
{
	if (!checkIsLight(node))
	{
		return;
	}
	MFnDependencyNode lightNode(node);
	FireMaya::ToonMaterial* thisNode = (FireMaya::ToonMaterial*)clientData;
	MGlobal::executeCommand("ToonShaderLightAdded " + lightNode.name() + " " + thisNode->name());
}

void FireMaya::ToonMaterial::onLightRemoved(MObject& node, void* clientData)
{
	if (!checkIsLight(node))
	{
		return;
	}
	MFnDependencyNode lightNode(node);
	FireMaya::ToonMaterial* thisNode = (FireMaya::ToonMaterial*)clientData;
	MGlobal::executeCommand("ToonShaderLightRemoved " + lightNode.name() + " " + thisNode->name());
}

void FireMaya::ToonMaterial::onLightRenamed(MObject& node, const MString& prevName, void* clientData)
{
	if (!checkIsLight(node) || prevName == NULL)
	{
		return;
	}
	MFnDependencyNode lightNode(node);
	MString name = lightNode.name();
	FireMaya::ToonMaterial* thisNode = (FireMaya::ToonMaterial*)clientData;
	if (lightNode.name() != prevName)
	{
		MGlobal::executeCommand("ToonShaderLightRenamed " + lightNode.name() + " " + prevName + " " + thisNode->name());
	}
}

FireMaya::ToonMaterial::~ToonMaterial()
{
	if (nodeAddedCallback != 0)
	{
		MNodeMessage::removeCallback(nodeAddedCallback);
	}
	if (nodeRemovedCallback != 0)
	{
		MNodeMessage::removeCallback(nodeRemovedCallback);
	}
	if (nodeRenamedCallback != 0)
	{
		MNodeMessage::removeCallback(nodeRenamedCallback);
	}
}
