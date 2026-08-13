/*
-------- Unprojecting mouse coordinate (NDC) to world space 
float ndcX = (2.0f * mouseX) / screenWidth - 1.0f;
float ndcY = 1.0f - (2.0f * mouseY) / screenHeight;
XMMATRIX viewProjectionMatrix = XMMatrixMultiply(viewMatrix, projectionMatrix);

XMVECTOR mouseNDC = XMVectorSet(ndcX, ndcY, 0.0 , 1.0); // Z value stands for Near/Far plane 

XMMATRIX inverseViewProjection = XMMatrixInverse(nullptr, viewProjectionMatrix);

XMVECTOR worldNear = XMVector3TransformCoord(mouseNear, inverseViewProjection);
*/