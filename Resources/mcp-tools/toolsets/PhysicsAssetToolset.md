# PhysicsToolsets.PhysicsAssetToolset

17 个工具

| 工具名 | 说明 |
|--------|------|
| `SetSphere` | 在形体上添加或替换Sphere碰撞图元。
如果形体上已存在拥有给定名称的任何形状，则先将其移除。 |
| `SetConstraintLimits` | 更新现有约束的角限制。 |
| `SetCapsule` | 在形体上添加或替换胶囊体碰撞图元。
如果形体上已存在拥有给定名称的任何形状，则先将其移除。
应用旋转之后，胶囊体的长轴是其局部Z轴。 |
| `SetBox` | 在形体上添加或替换盒体碰撞图元。
如果形体上已存在拥有给定名称的任何形状，则先将其移除。 |
| `SetBodyPhysicsMode` | 设置给定形体的物理模拟模式。 |
| `SetBodyMassScale` | 设置给定形体的质量比例乘数。 |
| `RemoveShape` | 按名称从形体中移除碰撞图元。 |
| `RemoveConstraint` | 移除两个形体之间的约束。 |
| `RemoveBody` | 移除给定骨骼的形体以及引用它的所有约束。
如果PhysicsAsset为空或BoneName不存在形体，则引发脚本错误。 |
| `GetConstraints` | 返回物理资产中的所有约束及其当前角限制。 |
| `GetBodyShapes` | 返回分配给形体的所有碰撞形状。 |
| `GetBodyPhysicsMode` | 返回给定形体的物理模拟模式。 |
| `GetBodyNames` | 返回物理资产中每个刚体的骨骼名称。 |
| `GetBodyMassScale` | 返回给定形体的质量缩放倍数。 |
| `CreateFromMesh` | 从骨骼网格体创建物理资产，为每个骨骼自动生成
碰撞体。资产放置在与网格体相同的文件夹中，后缀为
“_PhysicsAsset”。 |
| `AddConstraint` | 在两个形体之间添加一个新约束。两个形体必须已经存在。 |
| `AddBody` | 为给定骨骼添加一个新的空形体。 |