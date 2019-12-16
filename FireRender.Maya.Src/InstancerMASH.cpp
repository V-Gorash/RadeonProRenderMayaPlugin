#include <InstancerMASH.h>
#include <FireRenderMeshMASH.h>

InstancerMASH::InstancerMASH(FireRenderContext* context, const MDagPath& dagPath) :
	FireRenderNode(context, dagPath),
	m_instancedObjectsCachedSize(0)
{
	GenerateInstances();
	RegisterCallbacks();
}

void InstancerMASH::RegisterCallbacks()
{
	AddCallback(MNodeMessage::addNodeDirtyPlugCallback(m.object, plugDirty_callback, this));
}

void InstancerMASH::Freshen()
{
	if (GetTargetObjects().empty())
	{
		return;
	}

	if (m_instancedObjects.empty())
	{
		GenerateInstances();
	}

	const MObject firstInstancedObject = GetTargetObjects().at(0);
	const FireRenderMesh* renderMesh = static_cast<FireRenderMesh*>(context()->getRenderObject(firstInstancedObject));

	//Target node translation shouldn't affect the result 
	MTransformationMatrix targetNodeMatrix = MFnTransform(MFnDagNode(renderMesh->Object()).parent(0)).transformation();
	targetNodeMatrix.setTranslation({ 0., 0., 0. }, MSpace::kObject);

	MTransformationMatrix instancerMatrix = MFnTransform(m.object).transformation();
	std::vector<MMatrix> matricesFromMASH = GetTransformMatrices();

	for (size_t i = 0; i < GetInstanceCount(); i++)
	{
		MMatrix newTransform = targetNodeMatrix.asMatrix();
		newTransform *= matricesFromMASH.at(i);
		newTransform *= instancerMatrix.asMatrix();

		auto instancedObject = m_instancedObjects.at(i);
		instancedObject->SetSelfTransform(newTransform);
		instancedObject->Rebuild();
		instancedObject->setDirty();
	}

	m_instancedObjectsCachedSize = GetInstanceCount();
}

void InstancerMASH::OnPlugDirty(MObject& node, MPlug& plug)
{
	(void) node;
	(void) plug;
	if (ShouldBeRecreated())
	{
		for (auto o : m_instancedObjects)
		{
			o.second->setVisibility(false);
		}
		m_instancedObjects.clear();
	}
	setDirty();
}

size_t InstancerMASH::GetInstanceCount() const
{
	MPlug instanceCountPlug(m.object, MFnDependencyNode(m.object).attribute("instanceCount"));
	MInt64 instanceCount;
	instanceCountPlug.getValue(instanceCount);
	return static_cast<size_t>(instanceCount);
}

std::vector<MObject> InstancerMASH::GetTargetObjects() const
{
	MFnDependencyNode instancerDagNode(m.object);
	MPlugArray dagConnections;
	instancerDagNode.getConnections(dagConnections);

	std::vector<MObject> targetObjects;
	targetObjects.reserve(GetInstanceCount());

	// Sometimes here appear empty input hierarchy nodes. 
	for (const MPlug connection : dagConnections)
	{
		const std::string name(connection.partialName().asChar());
		if (name.find("inh[") == std::string::npos)
		{
			continue;
		}

		MPlugArray connectedTo;
		connection.connectedTo(connectedTo, true, false);
		for (const MPlug instanceConnection : connectedTo)
		{
			MFnDagNode node(instanceConnection.node());
			MObject mesh = node.child(0);
			targetObjects.push_back(mesh);
		}
	}

	return targetObjects;
}

std::vector<MMatrix> InstancerMASH::GetTransformMatrices() const
{
	std::vector<MMatrix> result;
	result.reserve(GetInstanceCount());

	MFnDependencyNode instancerDagNode(m.object);
	MPlug plug(m.object, instancerDagNode.attribute("inp"));
	MObject data = plug.asMDataHandle().data();
	MFnArrayAttrsData arrayAttrsData(data);

	MVectorArray positionData = arrayAttrsData.getVectorData("position");
	MVectorArray rotationData = arrayAttrsData.getVectorData("rotation");
	MVectorArray scaleData = arrayAttrsData.getVectorData("scale");

	for (unsigned i = 0; i < static_cast<unsigned>(GetInstanceCount()); i++)
	{
		MVector position = positionData[i];
		MVector rotation = rotationData[i];
		MVector scale = scaleData[i];

		double rotationRadiansArray[] = { deg2rad(rotation.x), deg2rad(rotation.y), deg2rad(rotation.z) };
		double scaleArray[] = { scale.x, scale.y, scale.z };

		MTransformationMatrix transformFromMASH;
		transformFromMASH.setScale(scaleArray, MSpace::Space::kWorld);
		transformFromMASH.setRotation(rotationRadiansArray, MTransformationMatrix::RotationOrder::kXYZ);
		transformFromMASH.setTranslation(position, MSpace::Space::kWorld);

		result.push_back(transformFromMASH.asMatrix());
	}

	return result;
}

void InstancerMASH::GenerateInstances()
{
	//Generate unique uuid, because we can't use instancer uuid - it initiates infinite Freshen() on whole hierarchy
	MUuid uuid;
	uuid.generate();

	//Generate instances with almost copy constructor with custom uuid passed
	const auto firstInstancedObject = GetTargetObjects().at(0);
	for (size_t i = 0; i < GetInstanceCount(); i++)
	{
		FireRenderMesh* renderMesh = static_cast<FireRenderMesh*>(context()->getRenderObject(firstInstancedObject));
		auto instance = std::make_shared<FireRenderMeshMASH>(*renderMesh, uuid.asString().asChar(), m.object);
		m_instancedObjects[i] = instance;
	}
}

bool InstancerMASH::ShouldBeRecreated() const
{
	return
		(GetInstanceCount() != m_instancedObjectsCachedSize)
		||
		GetTargetObjects().empty();
}